/* R73 SOLO ONLINE: authenticated, self-reported solo records. No peer-verified stats modified.
 * Included AFTER account/social and game stats helpers in xbox_netplay.cpp. */
static bool gR73SoloActive=false;
static char gR73SoloSid[40]={0};
static unsigned gR73SoloSeq=0,gR73SoloPendingSeq=0,gR73SoloPendingValue=0;
static int gR73SoloPendingCourse=0;
static char gR73SoloPendingMode[4]={0};
static DWORD gR73SoloPingAt=0,gR73SoloRetryAt=0;
static unsigned gR73SoloCounter=0;

extern "C" int xbox_netplay_solo_active(void){return gR73SoloActive?1:0;}
extern "C" void xbox_netplay_solo_finish(int mode,int course,int value){
    if(!gR73SoloActive||gR73SoloPendingSeq||course<0||course>18)return;
    if(mode==1){if(value<10000||value>3599999)return;strcpy(gR73SoloPendingMode,"TT");}
    else if(mode==2){if(value<0||value>7)return;strcpy(gR73SoloPendingMode,"GP");}
    else return;
    gR73SoloPendingCourse=course;gR73SoloPendingValue=(unsigned)value;
    gR73SoloPendingSeq=++gR73SoloSeq;gR73SoloRetryAt=0;
}
static void r73_solo_exit(void){
    if(!gR73SoloActive)return;
    R48AccountOG *me=r48_me();
    if(me&&gR48DirSocket!=INVALID_SOCKET){char m[160];
        snprintf(m,sizeof(m)-1,"MKDIR2|SOLO_EXIT|%s|%s|%s",me->id,me->secret,gR73SoloSid);
        m[sizeof(m)-1]=0;r48_send(m);
    }
    gR73SoloActive=false;gR73SoloSid[0]=0;gR73SoloPendingSeq=0;
}
static bool r73_solo_begin(void){
    if(gActive||gR73SoloActive||!r48_me()||!r48_dir_open())return false;
    R48AccountOG *me=r48_me();char m[220],response[256];
    unsigned gpRaces=0,gpWins=0,ttTracks=0;
    snprintf(m,sizeof(m)-1,"MKDIR2|SOLO_SUMMARY|%s|%s",me->id,me->secret);m[sizeof(m)-1]=0;
    response[0]=0;
    if(r48_wait_prefix(m,"MKDIR2|SOLO_SUMMARY|",response,sizeof(response),1800U))
        sscanf(response,"MKDIR2|SOLO_SUMMARY|%u|%u|%u",&gpRaces,&gpWins,&ttTracks);
    ui_consume();
    for(;;){char gp[72],tt[72];
        snprintf(gp,sizeof(gp)-1,"SOLO GP: %u RACES / %u WINS",gpRaces,gpWins);
        snprintf(tt,sizeof(tt)-1,"TIME TRIAL TRACK BESTS: %u / 16",ttTracks);
        ui_screen("SOLO ONLINE",gp,tt,"TT TIMES: SELF-REPORTED","NO SECOND CONSOLE REQUIRED","A START  B BACK");
        unsigned q=ui_pressed();if(q&CONT_B){ui_consume();return false;}
        if(q&CONT_A){ui_consume();break;}Sleep(16);
    }
    ++gR73SoloCounter;
    snprintf(m,sizeof(m)-1,"MKDIR2|SOLO_OPEN|%s|%s|OG%08lX%04X|OG|%s",
             me->id,me->secret,(unsigned long)GetTickCount(),gR73SoloCounter&0xffffU,gR484Region);
    m[sizeof(m)-1]=0;response[0]=0;
    if(!r48_wait_prefix(m,"MKDIR2|SOLO_OPEN_OK|",response,sizeof(response),4500U)||
       sscanf(response,"MKDIR2|SOLO_OPEN_OK|%39s",gR73SoloSid)!=1){
        ui_screen("SOLO ONLINE UNAVAILABLE","HUB NEEDS R2.19 SOLO SUPPORT",
                  "NO SOLO RESULTS WILL BE RECORDED","","","A OR B BACK");
        r49_wait_a_or_b();return false;
    }
    gR73SoloActive=true;gR73SoloSeq=0;gR73SoloPendingSeq=0;
    gR73SoloPingAt=gR73SoloRetryAt=0;
    gR57PresenceActive=false;gR59WorldUiActive=false;
    ui_screen("SOLO CONNECTED","PLAY GRAND PRIX OR TIME TRIALS",
              "SOLO GP + TT BESTS TRACKED","TT RECORDS: SELF-REPORTED","","STARTING MARIO KART 64");
    Sleep(350);ui_consume();return true;
}
static void r73_solo_tick(void){
    if(!gR73SoloActive)return;
    R48AccountOG *me=r48_me();if(!me||gR48DirSocket==INVALID_SOCKET)return;
    DWORD now=GetTickCount();
    if(!gR73SoloPingAt||now-gR73SoloPingAt>=4000U){
        char m[220];snprintf(m,sizeof(m)-1,"MKDIR2|SOLO_PING|%s|%s|%s|OG|%s",
            me->id,me->secret,gR73SoloSid,gR484Region);m[sizeof(m)-1]=0;
        r48_send(m);gR73SoloPingAt=now;
    }
    if(gR73SoloPendingSeq&&(!gR73SoloRetryAt||now-gR73SoloRetryAt>=900U)){
        char m[240];snprintf(m,sizeof(m)-1,"MKDIR2|SOLO_RESULT|%s|%s|%s|%u|%s|%d|%u",
            me->id,me->secret,gR73SoloSid,gR73SoloPendingSeq,gR73SoloPendingMode,
            gR73SoloPendingCourse,gR73SoloPendingValue);m[sizeof(m)-1]=0;
        r48_send(m);gR73SoloRetryAt=now;
    }
    for(int i=0;i<12;++i){char b[512];sockaddr_in from;int flen=sizeof(from);
        int n=recvfrom(gR48DirSocket,b,sizeof(b)-1,0,(sockaddr*)&from,&flen);
        if(n<=0)break;b[n]=0;
        char sid[40]={0};unsigned seq=0;
        if(sscanf(b,"MKDIR2|SOLO_RESULT_OK|%39[^|]|%u",sid,&seq)==2){
            if(!strcmp(sid,gR73SoloSid)&&seq==gR73SoloPendingSeq)
                gR73SoloPendingSeq=0;
        }else if(!strncmp(b,"MKDIR2|STATUS|",14))r57_parse_status(b);
        else if(r59_world_packet(b)||r61_social_packet(b)){}
    }
}
