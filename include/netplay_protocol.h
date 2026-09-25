#ifndef MK64_NETPLAY_PROTOCOL_H
#define MK64_NETPLAY_PROTOCOL_H
#include <stdint.h>
#include <string.h>

/*
 * MK64 Xbox / Xbox 360 crossplay protocol v10
 *
 * Topology:
 *   P1 = host
 *   Remaining racers = remote consoles with one or two contiguous slots
 *
 * Clients send only their reserved racers' delayed input history to the host.
 * The host relays authoritative all-player frame sets back to every client.
 * Every machine runs the same deterministic simulation in lockstep.
 *
 * Wire encoding is explicit: never transmit native structures or pointers.
 */
namespace mknet {

enum {
    VERSION=10,
    BUILD=0xBA100922,
    /* BA10: 360-host state sync remains authoritative through race loading/countdown; race hash begins only at RACE_IN_PROGRESS. */
    HEADER=28,
    HISTORY=256,
    REDUNDANCY=24,
    MAX_DELAY=12,
    MAX_PLAYERS=8,
    CROSS_STATE_BYTES=1440,
    MAX_PACKET=1536
};

/* Separate wire signatures prevent a 2-4 lobby accepting a 4-8 client. */
inline unsigned &lobby_capacity(){static unsigned value=4;return value;}
/* A two-controller reservation is atomic, including handshake restarts. */
inline bool reservation_fits(unsigned used,unsigned previous,unsigned requested,unsigned capacity){
    return used>=1 && used<=capacity && previous<used && requested>=1 && requested<=2 && used-previous+requested<=capacity;
}
inline const char *wire_magic(){return lobby_capacity()==8?"MK8P":"MK4P";}
enum Platform { PLATFORM_UNKNOWN=0, PLATFORM_OG_XBOX=1, PLATFORM_XBOX360=2 };
enum Type {
    HELLO=1,
    OFFER,
    READY,
    START,
    START_ACK,
    CLIENT_INPUT,
    FRAMESET,
    GOODBYE,
    BOOT_READY,
    BOOT_GO,
    JOIN_REJECT,
    STATE_SYNC
};

struct Pad {
    uint16_t buttons;
    int8_t x,y;
};

inline uint32_t get32(const uint8_t *p) {
    return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];
}
inline void put32(uint8_t *p,uint32_t v) {
    p[0]=uint8_t(v>>24);p[1]=uint8_t(v>>16);p[2]=uint8_t(v>>8);p[3]=uint8_t(v);
}
inline bool equal(Pad a,Pad b) {
    return a.buttons==b.buttons&&a.x==b.x&&a.y==b.y;
}
inline void encode_pad(uint8_t *p,Pad a) {
    p[0]=uint8_t(a.buttons>>8);p[1]=uint8_t(a.buttons);p[2]=uint8_t(a.x);p[3]=uint8_t(a.y);
}
inline Pad decode_pad(const uint8_t *p) {
    Pad a={uint16_t(uint16_t(p[0])<<8|p[1]),int8_t(p[2]),int8_t(p[3])};return a;
}

inline int header(uint8_t *p,Type t,const uint8_t session[16],int payload) {
    memset(p,0,HEADER);
    memcpy(p,wire_magic(),4);
    p[4]=VERSION;
    p[5]=uint8_t(t);
    p[6]=uint8_t((HEADER+payload)>>8);
    p[7]=uint8_t(HEADER+payload);
    put32(p+8,uint32_t(BUILD));
    memcpy(p+12,session,16);
    return HEADER+payload;
}

inline bool valid(const uint8_t *p,int n) {
    if(n<(int)HEADER||n>(int)MAX_PACKET||memcmp(p,wire_magic(),4)||p[4]!=VERSION||get32(p+8)!=(uint32_t)BUILD)return false;
    if((int(p[6])*256+p[7])!=n)return false;
    int payload=n-HEADER;
    const uint8_t *q=p+HEADER;
    switch(p[5]) {
    case HELLO:
        return payload==2 && q[0]>=1 && q[0]<=2 &&
               (q[1]==PLATFORM_OG_XBOX || q[1]==PLATFORM_XBOX360);
    case JOIN_REJECT:
        return payload==1 && q[0]>=1 && q[0]<=2;
    case GOODBYE:
    case BOOT_READY:
    case BOOT_GO:
        return payload==0;
    case OFFER:
    case READY:
        return payload==24 && q[20]>=1 && q[22]<=1 && unsigned(q[20])+q[22]<lobby_capacity() &&
               (q[23]==PLATFORM_OG_XBOX || q[23]==PLATFORM_XBOX360);
    case START:
        return payload==6 && q[3]<=1 && q[4]<=1 && q[5]<=1 && (!q[3] || q[4]) && q[0]>=2 && q[0]<=MAX_DELAY &&
               q[1]>=2 && q[1]<=lobby_capacity() &&
               q[2]>=1 && q[2]+q[3]<q[1];
    case START_ACK:
        return payload==1 && q[0]>=1 && q[0]<lobby_capacity();
    case CLIENT_INPUT: {
        if(payload<20)return false;
        unsigned slot=q[0],count=q[1];
        /* Slot 0 is valid in 2P when the host sends its early input
         * directly to the guest. */
        return q[3]<=1 && slot+q[3]<lobby_capacity()&&count>0&&count<=REDUNDANCY&&
               payload==16+4*int(count)*int(q[3]+1);
    }
    case FRAMESET: {
        if(payload<24)return false;
        unsigned players=q[0],count=q[1];
        return players>=2&&players<=lobby_capacity()&&count>0&&count<=REDUNDANCY&&
               payload==16+4*int(players)*int(count);
    }
    case STATE_SYNC:
        return payload==4+CROSS_STATE_BYTES;
    default:
        return false;
    }
}

/* No input frame may advance until every game thread reaches its first read. */
struct BootBarrier {
    unsigned players,mask;
    bool complete;
    void reset(unsigned p){players=p;mask=1;complete=false;}
    bool ready(unsigned slot){
        if(players<2||players>MAX_PLAYERS||slot==0||slot>=players)return false;
        mask|=1U<<slot;return true;
    }
    bool ready_span(unsigned slot,unsigned count){
        if(count<1||count>2||slot==0||slot+count>players)return false;
        for(unsigned i=0;i<count;++i) ready(slot+i);
        return true;
    }
    bool all_ready() const{return players>=2&&players<=MAX_PLAYERS&&mask==((1U<<players)-1);}
};

/* Integer EWMA and variation; do not size the buffer from the luckiest ping. */
struct Latency {
    unsigned mean,variation,samples;
    void add(unsigned ms) {
        /* A first RTT sample tells us the mean, but not the jitter. Treating
         * half the RTT as initial variation made the first budget ~= 3x RTT. */
        if(!samples){mean=ms;variation=0;samples=1;return;}
        unsigned delta=ms>mean?ms-mean:mean-ms;
        variation=(3*variation+delta+2)/4;
        mean=(7*mean+ms+4)/8;
        if(samples<0xFFFFU)++samples;
    }
    unsigned budget() const {return mean+4*variation;}
};
inline unsigned input_delay(unsigned budget_ms) {
    /* 3P/4P keep the conservative host-relay budget. */
    unsigned d=(budget_ms*30+999)/1000+2;
    return d<2?2:d>MAX_DELAY?MAX_DELAY:d;
}
inline unsigned input_delay_2p(unsigned budget_ms) {
    /* Direct host->guest early input means one WAN crossing is on the
     * critical path. Convert roughly one-way RTT to 30 Hz frames and keep
     * one extra safety frame. */
    unsigned d=(budget_ms*30+1999)/2000+1;
    return d<2?2:d>MAX_DELAY?MAX_DELAY:d;
}
inline unsigned input_delay_early_relay(unsigned worst_ms,unsigned second_ms) {
    /* With 3P/4P early relay, the longest guest-to-guest path is approximately
     * guest A -> host -> guest B: half of each peer's host RTT budget. */
    unsigned path_ms=(worst_ms+second_ms+1)/2;
    unsigned d=(path_ms*30+999)/1000+1;
    return d<2?2:d>MAX_DELAY?MAX_DELAY:d;
}

struct InputSlot {
    uint32_t frame;
    Pad pad;
    bool present;
};

struct HashSlot {
    uint32_t frame,value;
    bool present;
};

struct Stream4 {
    InputSlot inputs[MAX_PLAYERS][HISTORY];
    HashSlot hashes[HISTORY];
    HashSlot peer_hashes[MAX_PLAYERS][HISTORY];
    uint32_t frame,latest_local,latest_complete;
    uint32_t peer_frame[MAX_PLAYERS];
    unsigned delay,players,local_slot,local_count;
    bool fault;

    void reset(unsigned d,unsigned p,unsigned slot,unsigned locals=1) {
        memset(this,0,sizeof(*this));
        if(d<2||d>MAX_DELAY||p<2||p>MAX_PLAYERS||locals<1||locals>2||slot+locals>p){fault=true;return;}
        delay=d;
        players=p;
        local_slot=slot;local_count=locals;
        Pad zero={0,0,0};
        for(unsigned f=0;f<d;++f) {
            for(unsigned s=0;s<p;++s) {
                InputSlot &in=inputs[s][f%HISTORY];
                in.present=true;
                in.frame=f;
                in.pad=zero;
            }
        }
        latest_local=d-1;
        latest_complete=d-1;
    }

    bool local_hash_matches(unsigned peer_slot,uint32_t f) {
        if(peer_slot>=MAX_PLAYERS)return false;
        const HashSlot &a=hashes[f%HISTORY];
        const HashSlot &b=peer_hashes[peer_slot][f%HISTORY];
        return !a.present||!b.present||a.frame!=f||b.frame!=f||a.value==b.value;
    }

    void check_all_hashes(uint32_t f) {
        for(unsigned s=0;s<players;++s) {
            if(s==local_slot)continue;
            if(!local_hash_matches(s,f))fault=true;
        }
    }

    void sample_local(Pad p,uint32_t state) {
        if(local_count!=1){fault=true;return;}
        sample_locals(&p,state);
    }

    void sample_locals(const Pad *pads,uint32_t state) {
        if(fault||local_slot+local_count>players)return;
        uint32_t f=frame+delay;
        for(unsigned i=0;i<local_count;++i){
            InputSlot &s=inputs[local_slot+i][f%HISTORY];
            if(s.present&&s.frame==f&&!equal(s.pad,pads[i])){fault=true;return;}
            s.present=true;s.frame=f;s.pad=pads[i];
        }
        latest_local=f;

        HashSlot &h=hashes[frame%HISTORY];
        h.frame=frame;h.value=state;h.present=true;
        check_all_hashes(frame);

        if(local_slot==0)update_complete();
    }

    bool all_present(uint32_t f) const {
        for(unsigned s=0;s<players;++s) {
            const InputSlot &in=inputs[s][f%HISTORY];
            if(!in.present||in.frame!=f)return false;
        }
        return true;
    }

    void update_complete() {
        if(local_slot!=0)return;
        while(latest_complete<0x7FFFFEFEU) {
            uint32_t next=latest_complete+1;
            if(!all_present(next))break;
            latest_complete=next;
        }
    }

    int client_packet(uint8_t *p,const uint8_t session[16],unsigned target_slot=0) {
        if(fault||target_slot>=players||target_slot==local_slot)return 0;
        uint32_t first=latest_local>=REDUNDANCY-1?latest_local-(REDUNDANCY-1):0;
        /* A peer's frame is the next input it needs, not a receipt timestamp.
         * Replay from that frame when a gap falls outside the usual tail. */
        if(peer_frame[target_slot]<first)first=peer_frame[target_slot];
        if(latest_local-first>=HISTORY){fault=true;return 0;}
        unsigned count=latest_local-first+1;
        if(count>REDUNDANCY)count=REDUNDANCY;
        int n=header(p,CLIENT_INPUT,session,16+count*4*local_count);
        uint8_t *q=p+HEADER;
        q[0]=uint8_t(local_slot);
        q[1]=uint8_t(count);
        const HashSlot &h=hashes[frame%HISTORY];
        q[2]=uint8_t((h.present&&h.frame==frame)?1:0);
        q[3]=uint8_t(local_count-1);
        put32(q+4,first);
        put32(q+8,frame);
        put32(q+12,q[2]?h.value:0);
        for(unsigned i=0;i<count;++i) {
            for(unsigned j=0;j<local_count;++j){
                const InputSlot &in=inputs[local_slot+j][(first+i)%HISTORY];
                encode_pad(q+16+(i*local_count+j)*4,in.pad);
            }
        }
        return n;
    }

    bool receive_remote(unsigned expected_slot,const uint8_t *p,int n,unsigned owned_count=1) {
        /* Host receives guest input as before. In 2P the guest also accepts
         * slot 0 directly, avoiding the guest->host->guest relay path. */
        if(fault||owned_count<1||owned_count>2||expected_slot+owned_count>players||
           (expected_slot<local_slot+local_count && expected_slot+owned_count>local_slot))return false;
        if(!valid(p,n)||p[5]!=CLIENT_INPUT)return false;
        const uint8_t *q=p+HEADER;
        if(unsigned(q[0])!=expected_slot||unsigned(q[3])+1U!=owned_count)return false;

        unsigned count=q[1];
        uint32_t first=get32(q+4),hf=get32(q+8);
        if(first>0x7FFFFF00U||hf>frame+HISTORY-1||first+count-1>frame+delay+REDUNDANCY)return false;

        for(unsigned i=0;i<count;++i) {
            uint32_t f=first+i;
            if(f<frame||f>frame+delay+REDUNDANCY)continue;
            for(unsigned j=0;j<owned_count;++j){
                InputSlot &s=inputs[expected_slot+j][f%HISTORY];
                Pad a=decode_pad(q+16+(i*owned_count+j)*4);
                if(s.present&&s.frame==f&&!equal(s.pad,a)){fault=true;return false;}
                s.present=true;s.frame=f;s.pad=a;
            }
        }

        if(q[2]&&hf+HISTORY>frame) {
            HashSlot &h=peer_hashes[expected_slot][hf%HISTORY];
            if(!h.present||hf>=h.frame){h.frame=hf;h.value=get32(q+12);h.present=true;}
            if(!local_hash_matches(expected_slot,hf))fault=true;
        }

        for(unsigned j=0;j<owned_count;++j)
            if(hf>peer_frame[expected_slot+j])peer_frame[expected_slot+j]=hf;
        update_complete();
        return !fault;
    }

    int frameset_packet(uint8_t *p,const uint8_t session[16],unsigned target_slot=1) {
        update_complete();
        uint32_t first=latest_complete>=REDUNDANCY-1?latest_complete-(REDUNDANCY-1):0;
        if(target_slot==0||target_slot>=players)return 0;
        if(peer_frame[target_slot]<first)first=peer_frame[target_slot];
        if(latest_complete-first>=HISTORY){fault=true;return 0;}
        unsigned count=latest_complete-first+1;
        if(count>REDUNDANCY)count=REDUNDANCY;
        int n=header(p,FRAMESET,session,16+count*players*4);
        uint8_t *q=p+HEADER;
        q[0]=uint8_t(players);
        q[1]=uint8_t(count);
        const HashSlot &h=hashes[frame%HISTORY];
        q[2]=uint8_t((h.present&&h.frame==frame)?1:0);
        q[3]=0;
        put32(q+4,first);
        put32(q+8,frame);
        put32(q+12,q[2]?h.value:0);

        uint8_t *out=q+16;
        for(unsigned i=0;i<count;++i) {
            uint32_t f=first+i;
            for(unsigned s=0;s<players;++s) {
                encode_pad(out,inputs[s][f%HISTORY].pad);
                out+=4;
            }
        }
        return n;
    }

    bool receive_frameset(const uint8_t *p,int n) {
        if(fault||local_slot==0)return false;
        if(!valid(p,n)||p[5]!=FRAMESET)return false;
        const uint8_t *q=p+HEADER;
        if(q[0]!=players)return false;

        unsigned count=q[1];
        uint32_t first=get32(q+4),hf=get32(q+8);
        if(first>0x7FFFFF00U||hf>frame+HISTORY-1||first+count-1>frame+delay+REDUNDANCY)return false;

        const uint8_t *in=q+16;
        for(unsigned i=0;i<count;++i) {
            uint32_t f=first+i;
            for(unsigned s=0;s<players;++s) {
                Pad a=decode_pad(in);in+=4;
                if(f<frame||f>frame+delay+REDUNDANCY)continue;
                InputSlot &dst=inputs[s][f%HISTORY];
                if(dst.present&&dst.frame==f&&!equal(dst.pad,a)){fault=true;return false;}
                dst.present=true;dst.frame=f;dst.pad=a;
            }
        }

        if(q[2]&&hf+HISTORY>frame) {
            HashSlot &h=peer_hashes[0][hf%HISTORY];
            if(!h.present||hf>=h.frame){h.frame=hf;h.value=get32(q+12);h.present=true;}
            if(!local_hash_matches(0,hf))fault=true;
        }
        if(hf>peer_frame[0])peer_frame[0]=hf;
        return !fault;
    }

    bool consume(Pad out[MAX_PLAYERS]) {
        if(fault||frame>=0x7FFFFF00U)return false;
        for(unsigned s=0;s<players;++s) {
            InputSlot &in=inputs[s][frame%HISTORY];
            if(!in.present||in.frame!=frame)return false;
        }
        for(unsigned s=0;s<players;++s)out[s]=inputs[s][frame%HISTORY].pad;
        ++frame;
        return true;
    }
};

inline bool parse_endpoint(const char *s,uint32_t &ip,uint16_t &port) {
    ip=0;port=6464;
    for(int i=0;i<4;++i) {
        unsigned v=0,d=0;
        while(*s>='0'&&*s<='9') {
            v=v*10+(*s++-'0');
            if(++d>3||v>255)return false;
        }
        if(!d)return false;
        ip=(ip<<8)|v;
        if(i<3&&*s++!='.')return false;
    }
    if(*s==':') {
        unsigned v=0,d=0;++s;
        while(*s>='0'&&*s<='9') {
            v=v*10+(*s++-'0');
            if(++d>5||v>65535)return false;
        }
        if(!d||!v)return false;
        port=uint16_t(v);
    }
    return !*s && (ip>>24)>0 && (ip>>24)<224 && ip!=0xFFFFFFFFU;
}

/* RFC 8489 IPv4 XOR-MAPPED-ADDRESS; match response transaction and exact length. */
inline bool stun_address(const uint8_t *p,int n,const uint8_t tx[12],uint32_t &ip,uint16_t &port) {
    if(n<20||p[0]!=1||p[1]!=1||get32(p+4)!=0x2112A442U||memcmp(p+8,tx,12))return false;
    unsigned size=unsigned(p[2])*256+p[3];
    if(size%4||size+20!=unsigned(n))return false;
    for(unsigned off=20;off+4<=unsigned(n);) {
        unsigned type=unsigned(p[off])*256+p[off+1];
        unsigned len=unsigned(p[off+2])*256+p[off+3];
        off+=4;
        if(len>unsigned(n)-off)return false;
        if(type==0x20&&len==8&&p[off+1]==1) {
            port=uint16_t((unsigned(p[off+2])*256+p[off+3])^0x2112);
            ip=get32(p+off+4)^0x2112A442U;
            return port!=0;
        }
        off+=(len+3)&~3U;
    }
    return false;
}

} /* namespace mknet */
#endif
