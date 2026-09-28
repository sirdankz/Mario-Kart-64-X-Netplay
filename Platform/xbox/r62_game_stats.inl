/* Copyright (c) 2026 Sirdankz; SPDX-License-Identifier: MPL-2.0
 * R62: authenticated, unranked Hub presence and peer-confirmed completed results.
 * Included after r61_social_shared.inl; no mutation of deterministic game state. */
#ifndef MK64_R62_GAME_STATS_INL
#define MK64_R62_GAME_STATS_INL
#ifdef R61_OG
#define R62_PLATFORM "OG"
#define R62_ROOM gR48RoomId
#define R62_SESSION gSession
#define R62_LOCAL_SLOT gLocalSlot
#define R62_LOCAL_COUNT gLocalCount
#define R62_PLAYERS gPlayerCount
#define R62_REGION gR484Region
#define R62_GAME_ACTIVE gActive
#define R62_LOG(...) log_line(__VA_ARGS__)
#else
#define R62_PLATFORM "X360"
#define R62_ROOM r48_room_id
#define R62_SESSION session
#define R62_LOCAL_SLOT local_slot
#define R62_LOCAL_COUNT local_count
#define R62_PLAYERS player_count
#define R62_REGION r484_region
#define R62_GAME_ACTIVE active
#define R62_LOG(...) net_log(__VA_ARGS__)
#endif
struct R62Event {bool valid,recorded;unsigned seq;int course,winner,cup;const char *mode;DWORD sent;};
static bool r62_game=false,r62_open_ok=false,r62_race_live=false,r62_race_done=false;
static unsigned r62_seq=0,r62_player_slots=0,r62_local_slot=0,r62_local_count=0;
static DWORD r62_open_sent=0,r62_ping_sent=0,r62_poll_sent=0;
static char r62_room[20]={0},r62_sid[33]={0};
enum {R62_EVENT_LIMIT=64};
static R62Event r62_events[R62_EVENT_LIMIT];
static void r62_begin_game(){
    if(!R62_GAME_ACTIVE||!r48_me()||!R62_ROOM[0]||R62_PLAYERS<2||R62_PLAYERS>4)return;
    for(int i=0;i<16;++i)R61_SNPRINTF(r62_sid+i*2,3,"%02x",(unsigned)R62_SESSION[i]);
    r62_sid[32]=0;r61_copy(r62_room,sizeof(r62_room),R62_ROOM);
    r62_player_slots=R62_PLAYERS;r62_local_slot=R62_LOCAL_SLOT;r62_local_count=R62_LOCAL_COUNT;
    memset(r62_events,0,sizeof(r62_events));r62_seq=0;r62_open_ok=false;r62_open_sent=r62_ping_sent=r62_poll_sent=0;
    r62_race_live=r62_race_done=false;r62_game=true;
    R62_LOG("R62_STATS: active room captured; auth/gameplay reporting enabled\n");
}
static void r62_observe_result(int race_state,int mode,int course,int cup,int winner){
    if(!r62_game||!R62_GAME_ACTIVE)return;
    if(race_state==3){
        if(!r62_race_live){if(r62_race_done){++r62_seq;r62_race_done=false;}r62_race_live=true;}
        return;
    }
    if(race_state!=5||!r62_race_live||r62_race_done)return;
    const char *name=mode==0?"GP":mode==2?"VS":mode==3?"BATTLE":0;
    if(!name||r62_seq>=R62_EVENT_LIMIT||course<0||course>=20||winner<0||(unsigned)winner>=r62_player_slots)return;
    R62Event &ev=r62_events[r62_seq];ev.valid=true;ev.recorded=false;ev.seq=r62_seq;
    ev.mode=name;ev.course=course;ev.winner=winner;ev.cup=mode==0?cup:-1;ev.sent=0;
    r62_race_done=true;r62_race_live=false;
    R62_LOG("R62_STATS: finished unranked %s round=%u course=%d winner-slot=%d queued\n",name,r62_seq,course,winner+1);
}
static void r62_game_send_open(){
    if(!r48_me())return;
    char q[240];R61_SNPRINTF(q,sizeof(q)-1,"MKDIR2|MATCH_OPEN|%s|%s|%s|%s|%u|%u|%u",
        r48_me()->id,r48_me()->secret,r62_room,r62_sid,r62_local_slot,r62_local_count,r62_player_slots);
    q[sizeof(q)-1]=0;r48_send(q);r62_open_sent=GetTickCount();
}
static void r62_game_send_pending(){
    if(!r62_open_ok||!r48_me())return;
    DWORD now=GetTickCount();
    for(int i=0;i<R62_EVENT_LIMIT;++i){R62Event &ev=r62_events[i];if(!ev.valid||ev.recorded)continue;
        if(ev.sent&&now-ev.sent<1100U)break;
        char q[260];R61_SNPRINTF(q,sizeof(q)-1,"MKDIR2|MATCH_ROUND|%s|%s|%s|%s|%u|%s|%d|%d|%d",
            r48_me()->id,r48_me()->secret,r62_room,r62_sid,ev.seq,ev.mode,ev.course,ev.winner,ev.cup);
        q[sizeof(q)-1]=0;r48_send(q);ev.sent=now;
        break; /* send at most one result packet per tick */
    }
}
static void r62_game_tick(){
    if(!r62_game||!R62_GAME_ACTIVE||!r48_me())return;
    if(!r48_dir_open())return;
    DWORD now=GetTickCount();
    if(!r62_open_ok&&(!r62_open_sent||now-r62_open_sent>=1100U))r62_game_send_open();
    if(r62_open_ok&&(!r62_ping_sent||now-r62_ping_sent>=5000U)){
        char q[240];R61_SNPRINTF(q,sizeof(q)-1,"MKDIR2|GAME_PING|%s|%s|%s|%s|%s|%s",
            r48_me()->id,r48_me()->secret,r62_room,r62_sid,R62_PLATFORM,R62_REGION);
        q[sizeof(q)-1]=0;r48_send(q);r62_ping_sent=now;
    }
    r62_game_send_pending();
    /* Avoid four nonblocking socket reads every simulation frame when idle.
     * Poll immediately while opening a match or awaiting result confirmation;
     * otherwise 100 ms is sufficient for informational Hub statuses. */
    bool waiting=!r62_open_ok;
    for(int i=0;i<R62_EVENT_LIMIT;++i)if(r62_events[i].valid&&!r62_events[i].recorded){waiting=true;break;}
    if(!waiting&&r62_poll_sent&&now-r62_poll_sent<100U)return;
    r62_poll_sent=now;
    sockaddr_in server;if(!r48_dir_addr(server))return;
    for(int i=0;i<4;++i){char b[280];sockaddr_in from;int flen=sizeof(from);
#ifdef R61_OG
        int n=recvfrom(gR48DirSocket,b,sizeof(b)-1,0,(sockaddr*)&from,&flen);
#else
        int n=recvfrom(r48_dir_sock,b,sizeof(b)-1,0,(sockaddr*)&from,&flen);
#endif
        if(n<=0)break;b[n]=0;
        if(from.sin_addr.s_addr!=server.sin_addr.s_addr||from.sin_port!=server.sin_port)continue;
        if(!strncmp(b,"MKDIR2|MATCH_OPEN_OK|",21)){
            if(strstr(b,r62_sid)){r62_open_ok=true;r62_ping_sent=0;}
        }else if(!strncmp(b,"MKDIR2|MATCH_ROUND_OK|",22)){
            unsigned seq=0;char status[20]={0};
            if(sscanf(b,"MKDIR2|MATCH_ROUND_OK|%u|%19s",&seq,status)==2&&seq<R62_EVENT_LIMIT&&!strcmp(status,"RECORDED")){
                r62_events[seq].recorded=true;
                R62_LOG("R62_STATS: unranked round=%u recorded by Hub\n",seq);
            }
        }else if(!strncmp(b,"MKDIR2|STATUS|",14)){
            /* Refresh cached counts without draining gameplay packets. */
#ifdef R61_OG
            r57_parse_status(b);
#else
            r57_parse_status(b);
#endif
        }else if(!strncmp(b,"MKDIR2|MATCH_",13)){
            R62_LOG("R62_STATS: Hub response %.80s\n",b);
        }
    }
}
static void r62_end_game(){
    if(!r62_game)return;
    if(r48_me()&&r48_dir_open()){
        /* Send any final completed race even if the user immediately relaunches
         * the title. The server never awards anything without peer agreement. */
        if(!r62_open_ok)r62_game_send_open();
        for(int i=0;i<R62_EVENT_LIMIT;++i){R62Event &ev=r62_events[i];if(!ev.valid||ev.recorded)continue;
            for(int j=0;j<2;++j){char q[260];R61_SNPRINTF(q,sizeof(q)-1,"MKDIR2|MATCH_ROUND|%s|%s|%s|%s|%u|%s|%d|%d|%d",
                r48_me()->id,r48_me()->secret,r62_room,r62_sid,ev.seq,ev.mode,ev.course,ev.winner,ev.cup);
                q[sizeof(q)-1]=0;r48_send(q);}
        }
        char q[190];R61_SNPRINTF(q,sizeof(q)-1,"MKDIR2|GAME_EXIT|%s|%s|%s|%s",
            r48_me()->id,r48_me()->secret,r62_room,r62_sid);q[sizeof(q)-1]=0;
        for(int j=0;j<3;++j)r48_send(q);
    }
    r62_game=false;r62_open_ok=false;r62_sid[0]=r62_room[0]=0;
    r48_dir_close();
}
#undef R62_PLATFORM
#undef R62_ROOM
#undef R62_SESSION
#undef R62_LOCAL_SLOT
#undef R62_LOCAL_COUNT
#undef R62_PLAYERS
#undef R62_REGION
#undef R62_GAME_ACTIVE
#undef R62_LOG
#endif
