/* Copyright (c) 2026 Sirdankz
 * SPDX-License-Identifier: MPL-2.0
 * Original netplay/social code; see NETPLAY-LICENSE.md.
 * MK64_R62_SOCIAL_STATS_AND_GAME_PRESENCE
 * MK64_R61_SOCIAL_UI_SHARED
 * Included after r48_me() in each platform's netplay translation unit.
 * Social traffic is authenticated on the dedicated Hub control socket.
 * Never alter deterministic gameplay input, state or title protocol. */
#ifndef R61_SOCIAL_SHARED_INL
#define R61_SOCIAL_SHARED_INL
#ifdef R61_OG
# define R61_SNPRINTF snprintf
# define R61_BUTTONS() ui_pressed()
# define R61_A CONT_A
# define R61_B CONT_B
# define R61_X CONT_X
# define R61_Y kUiYButton
# define R61_UP CONT_DPAD_UP
# define R61_DOWN CONT_DPAD_DOWN
# define R61_LEFT CONT_DPAD_LEFT
# define R61_RIGHT CONT_DPAD_RIGHT
# define R61_RS kUiRightStickButton
# define R61_STATS CONT_C
# define R61_STATS_LABEL "RT"
# define R61_CONSUME() ui_consume()
# define R61_SCREEN ui_screen
#else
# define R61_SNPRINTF _snprintf
# define R61_BUTTONS() pressed(false)
# define R61_A XINPUT_GAMEPAD_A
# define R61_B XINPUT_GAMEPAD_B
# define R61_X XINPUT_GAMEPAD_X
# define R61_Y XINPUT_GAMEPAD_Y
# define R61_UP XINPUT_GAMEPAD_DPAD_UP
# define R61_DOWN XINPUT_GAMEPAD_DPAD_DOWN
# define R61_LEFT XINPUT_GAMEPAD_DPAD_LEFT
# define R61_RIGHT XINPUT_GAMEPAD_DPAD_RIGHT
# define R61_RS XINPUT_GAMEPAD_RIGHT_THUMB
# define R61_CONSUME() r47_consume_buttons()
# define R61_SCREEN screen
#endif
struct R61Person {bool found;char id[20],name[20],platform[16],region[12];unsigned playing;};
struct R61Profile {bool found;char id[20],name[20],platform[16],region[12],joined[16],relation[16];unsigned online,friends,unread,requests,playing,gp_races,gp_wins,gp_cups,vs_races,vs_wins,battle_matches,battle_wins;};
struct R61Mail {bool found;char id[16],from[20],name[20],body[56];unsigned unread;};
struct R61Request {bool found;char id[20],name[20];};
static void r61_modal_service();
static R61Person r61_people[5];
static R61Profile r61_profile;
static R61Mail r61_mail[5];
static R61Request r61_requests[5];
static R61Request r61_friends[5];
static int r61_total=0,r61_offset=0,r61_mail_total=0,r61_mail_unread=0,r61_request_total=0,r61_friend_total=0;
static int r61_mail_offset=0,r61_request_offset=0,r61_friend_offset=0;
static bool r61_list_done=false,r61_mail_done=false,r61_request_done=false,r61_friend_done=false,r61_result_done=false;
static char r61_result_action[20]={0},r61_result_code[32]={0},r61_profile_wanted[20]={0};
static DWORD r61_last_list=0,r61_last_list_send=0;
static bool r61_match(const char *a,const char *b){return a&&b&&!strcmp(a,b);}
static void r61_copy(char *out,unsigned n,const char *v){
    if(!out||!n)return;
    unsigned i=0;if(v)for(;i+1<n&&v[i];++i)out[i]=v[i];out[i]=0;
}
static int r61_count_people(){int n=0;for(int i=0;i<5;++i)if(r61_people[i].found)++n;return n;}
static int r61_count_mail(){int n=0;for(int i=0;i<5;++i)if(r61_mail[i].found)++n;return n;}
static int r61_count_requests(){int n=0;for(int i=0;i<5;++i)if(r61_requests[i].found)++n;return n;}
static int r61_count_friends(){int n=0;for(int i=0;i<5;++i)if(r61_friends[i].found)++n;return n;}

/* Called by every existing directory-socket receive loop.  This is important:
 * world chat and room polls use the same UDP socket as profiles/mail. */
static bool r61_social_packet(const char *b){
    if(!b||strncmp(b,"MKDIR2|SOCIAL_",14))return false;
    if(r70_social_packet(b))return true;
    unsigned a=0,c=0,d=0,e=0;
    if(sscanf(b,"MKDIR2|SOCIAL_LISTBEGIN|%u|%u|%u",&a,&c,&d)>=2){
        r62_game_total=(int)d;
        /* A room list can shrink while the user is on a later page.  The Hub
         * clamps the requested page; follow it instead of getting stuck. */
        if((int)a!=r61_offset && (int)c<=r61_offset && (int)a<r61_offset)r61_offset=(int)a;
        if((int)a==r61_offset){memset(r61_people,0,sizeof(r61_people));r61_total=(int)c;r61_list_done=false;}
        return true;
    }
    if(!strncmp(b,"MKDIR2|SOCIAL_USER|",19)){
        char id[20]={0},name[20]={0},platform[16]={0},region[12]={0};
        unsigned game=0;
        int nf=sscanf(b,"MKDIR2|SOCIAL_USER|%u|%19[^|]|%19[^|]|%15[^|]|%11[^|]|%u",&a,id,name,platform,region,&game);
        if(nf<5)nf=sscanf(b,"MKDIR2|SOCIAL_USER|%u|%19[^|]|%19[^|]|%15[^|]|%11[^\r\n]",&a,id,name,platform,region);
        if(nf>=5){
            int idx=(int)a-r61_offset;if(idx>=0&&idx<5){R61Person &v=r61_people[idx];v.found=true;v.playing=game;r61_copy(v.id,sizeof(v.id),id);r61_copy(v.name,sizeof(v.name),name);r61_copy(v.platform,sizeof(v.platform),platform);r61_copy(v.region,sizeof(v.region),region);}
        }
        return true;
    }
    if(sscanf(b,"MKDIR2|SOCIAL_LISTEND|%u|%u",&a,&c)==2){
        if((int)a==r61_offset){r61_total=(int)c;int expected=(r61_total-r61_offset);if(expected<0)expected=0;if(expected>5)expected=5;r61_list_done=r61_count_people()>=expected;}
        return true;
    }
    if(!strncmp(b,"MKDIR2|SOCIAL_PROFILE|",22)){
        R61Profile p;memset(&p,0,sizeof(p));
        int fields=sscanf(b,"MKDIR2|SOCIAL_PROFILE|%19[^|]|%19[^|]|%u|%15[^|]|%11[^|]|%15[^|]|%u|%u|%15[^|]|%u|%u|%u|%u|%u|%u|%u|%u|%u",
            p.id,p.name,&a,p.platform,p.region,p.joined,&c,&d,p.relation,&e,&p.playing,&p.gp_races,&p.gp_wins,&p.gp_cups,&p.vs_races,&p.vs_wins,&p.battle_matches,&p.battle_wins);
        if(fields>=10&&r61_match(p.id,r61_profile_wanted)){
            p.found=true;p.online=a;p.friends=c;p.unread=d;p.requests=e;r61_profile=p;
        }
        return true;
    }
    if(sscanf(b,"MKDIR2|SOCIAL_MAILBEGIN|%u|%u",&a,&c)==2){if((int)c==r61_mail_offset){memset(r61_mail,0,sizeof(r61_mail));r61_mail_total=(int)a;r61_mail_done=false;}return true;}
    if(!strncmp(b,"MKDIR2|SOCIAL_MAIL|",19)){
        R61Mail m;memset(&m,0,sizeof(m));
        if(sscanf(b,"MKDIR2|SOCIAL_MAIL|%u|%15[^|]|%19[^|]|%19[^|]|%u|%55[^\r\n]",&a,m.id,m.from,m.name,&c,m.body)==6&&a<5){m.found=true;m.unread=c;r61_mail[a]=m;}
        return true;
    }
    if(sscanf(b,"MKDIR2|SOCIAL_MAILEND|%u|%u|%u",&a,&c,&d)==3){if((int)d==r61_mail_offset){r61_mail_total=(int)a;r61_mail_unread=(int)c;int want=r61_mail_total-r61_mail_offset;if(want<0)want=0;if(want>5)want=5;r61_mail_done=r61_count_mail()>=want;}return true;}
    if(sscanf(b,"MKDIR2|SOCIAL_REQBEGIN|%u|%u",&a,&c)==2){if((int)c==r61_request_offset){memset(r61_requests,0,sizeof(r61_requests));r61_request_total=(int)a;r61_request_done=false;}return true;}
    if(!strncmp(b,"MKDIR2|SOCIAL_REQUEST|",22)){
        char id[20]={0},name[20]={0};if(sscanf(b,"MKDIR2|SOCIAL_REQUEST|%u|%19[^|]|%19[^\r\n]",&a,id,name)==3&&a<5){R61Request &v=r61_requests[a];v.found=true;r61_copy(v.id,sizeof(v.id),id);r61_copy(v.name,sizeof(v.name),name);}return true;
    }
    if(sscanf(b,"MKDIR2|SOCIAL_REQEND|%u|%u",&a,&c)==2){if((int)c==r61_request_offset){r61_request_total=(int)a;int want=r61_request_total-r61_request_offset;if(want<0)want=0;if(want>5)want=5;r61_request_done=r61_count_requests()>=want;}return true;}
    if(sscanf(b,"MKDIR2|SOCIAL_FRIENDBEGIN|%u|%u",&a,&c)==2){if((int)c==r61_friend_offset){memset(r61_friends,0,sizeof(r61_friends));r61_friend_total=(int)a;r61_friend_done=false;}return true;}
    if(!strncmp(b,"MKDIR2|SOCIAL_FRIEND_USER|",26)){
        char id[20]={0},name[20]={0};if(sscanf(b,"MKDIR2|SOCIAL_FRIEND_USER|%u|%19[^|]|%19[^|]|%u",&a,id,name,&c)==4&&a<5){R61Request &v=r61_friends[a];v.found=true;r61_copy(v.id,sizeof(v.id),id);r61_copy(v.name,sizeof(v.name),name);}return true;
    }
    if(sscanf(b,"MKDIR2|SOCIAL_FRIENDEND|%u|%u",&a,&c)==2){if((int)c==r61_friend_offset){r61_friend_total=(int)a;int want=r61_friend_total-r61_friend_offset;if(want<0)want=0;if(want>5)want=5;r61_friend_done=r61_count_friends()>=want;}return true;}
    if(!strncmp(b,"MKDIR2|SOCIAL_RESULT|",21)){
        char kind[20]={0},code[32]={0};if(sscanf(b,"MKDIR2|SOCIAL_RESULT|%19[^|]|%31[^\r\n]",kind,code)==2){r61_copy(r61_result_action,sizeof(r61_result_action),kind);r61_copy(r61_result_code,sizeof(r61_result_code),code);r61_result_done=true;}return true;
    }
    return true;
}
static void r61_online_tick(bool force){
    if(!r48_me())return;
    DWORD now=GetTickCount();
    if(force||!r61_last_list||now-r61_last_list>=5000U){
        r61_last_list=now;r61_last_list_send=0;r61_list_done=false;memset(r61_people,0,sizeof(r61_people));
    }
    if(!r61_list_done&&(!r61_last_list_send||now-r61_last_list_send>=650U)){
        char q[160];R61_SNPRINTF(q,sizeof(q)-1,"MKDIR2|SOCIAL_ONLINE|%s|%s|%d",r48_me()->id,r48_me()->secret,r61_offset);q[sizeof(q)-1]=0;r48_send(q);r61_last_list_send=now;
    }
}
/* Blocking only in pregame profile/message screens; never in the simulation. */
static bool r61_wait(const char *q,const char *kind,DWORD timeout=2200U){
    r61_result_done=false;r61_result_action[0]=r61_result_code[0]=0;
    DWORD begin=GetTickCount(),last=0;
    while(GetTickCount()-begin<timeout){
        DWORD now=GetTickCount();if(!last||now-last>=420U){r48_send(q);last=now;}
        r57_presence_tick(true);r61_modal_service();
        if(r61_match(kind,"PROFILE")&&r61_profile.found)return true;
        if(r61_match(kind,"MAILBOX")&&r61_mail_done)return true;
        if(r61_match(kind,"REQUESTS")&&r61_request_done)return true;
        if(r61_match(kind,"FRIENDS")&&r61_friend_done)return true;
        if(r61_result_done&&r61_match(r61_result_action,kind))return r61_match(r61_result_code,"SENT")||r61_match(r61_result_code,"REQUEST_SENT")||r61_match(r61_result_code,"FRIEND_ADDED")||r61_match(r61_result_code,"ALREADY_FRIENDS")||r61_match(r61_result_code,"ALREADY_SENT")||r61_match(r61_result_code,"OK")||r61_match(r61_result_code,"REMOVED")||r61_match(r61_result_code,"CANCELED");
        Sleep(10);
    }
    return false;
}
static bool r61_fetch_profile(const char *id){
    if(!r48_me()||!id||!*id)return false;
    memset(&r61_profile,0,sizeof(r61_profile));r61_copy(r61_profile_wanted,sizeof(r61_profile_wanted),id);
    char q[160];R61_SNPRINTF(q,sizeof(q)-1,"MKDIR2|SOCIAL_PROFILE|%s|%s|%s",r48_me()->id,r48_me()->secret,id);q[sizeof(q)-1]=0;
    return r61_wait(q,"PROFILE");
}
static bool r61_fetch_mail(){
    if(!r48_me())return false;
    r61_mail_done=false;memset(r61_mail,0,sizeof(r61_mail));
    char q[160];R61_SNPRINTF(q,sizeof(q)-1,"MKDIR2|SOCIAL_MAILBOX|%s|%s|%d",r48_me()->id,r48_me()->secret,r61_mail_offset);q[sizeof(q)-1]=0;
    return r61_wait(q,"MAILBOX");
}
static bool r61_fetch_requests(){
    if(!r48_me())return false;
    r61_request_done=false;memset(r61_requests,0,sizeof(r61_requests));
    char q[160];R61_SNPRINTF(q,sizeof(q)-1,"MKDIR2|SOCIAL_REQUESTS|%s|%s|%d",r48_me()->id,r48_me()->secret,r61_request_offset);q[sizeof(q)-1]=0;
    return r61_wait(q,"REQUESTS");
}
static bool r61_fetch_friends(){
    if(!r48_me())return false;
    r61_friend_done=false;memset(r61_friends,0,sizeof(r61_friends));
    char q[160];R61_SNPRINTF(q,sizeof(q)-1,"MKDIR2|SOCIAL_FRIENDS|%s|%s|%d",r48_me()->id,r48_me()->secret,r61_friend_offset);q[sizeof(q)-1]=0;
    return r61_wait(q,"FRIENDS");
}
static bool r61_friend_action(const char *id,const char *action){
    if(!r48_me()||!id||!*id)return false;
    char q[180];R61_SNPRINTF(q,sizeof(q)-1,"MKDIR2|SOCIAL_FRIEND|%s|%s|%s|%s",r48_me()->id,r48_me()->secret,id,action);q[sizeof(q)-1]=0;
    return r61_wait(q,"FRIEND");
}
static bool r61_mail_compose(const char *id){
    if(!r48_me()||!id||!*id)return false;
    char body[56]={0};if(!r48_keyboard("PRIVATE MESSAGE",body,53,false,false))return false;
    char q[320];static unsigned serial=0;++serial;
    R61_SNPRINTF(q,sizeof(q)-1,"MKDIR2|SOCIAL_MAIL_SEND|%s|%s|%s|%08lX%04X|%s",r48_me()->id,r48_me()->secret,id,(unsigned long)GetTickCount(),serial&65535U,body);q[sizeof(q)-1]=0;
    return r61_wait(q,"MAIL");
}
static void r61_mailbox_view();
static void r61_request_view();
static void r61_friends_view();
static void r61_profile_view(const char *id,bool start_stats=false){
    if(!id||!*id)return;
    bool old=R61_WORLD_SUPPRESS;R61_WORLD_SUPPRESS=true;R61_CONSUME();
    if(!r61_fetch_profile(id)){R61_SCREEN("PROFILE UNAVAILABLE","NO RESPONSE FROM MARIO KART HUB","TRY AGAIN LATER","","","B BACK");Sleep(750);R61_WORLD_SUPPRESS=old;R61_CONSUME();return;}
    char wanted[20];r61_copy(wanted,sizeof(wanted),id);char notice[56]={0};int page=start_stats?1:0;
    for(;;){
        if(!r61_profile.found)break;
        bool self=r61_match(wanted,r48_me()->id);
        char title[72],a[96],b[96],c[96],d[96],footer[120];
        R61_SNPRINTF(title,sizeof(title)-1,page?"STATS: %s":"PROFILE: %s",r61_profile.name);
        if(page){
            R61_SNPRINTF(a,sizeof(a)-1,"GP: RACES %u  WINS %u  CUPS %u",r61_profile.gp_races,r61_profile.gp_wins,r61_profile.gp_cups);
            R61_SNPRINTF(b,sizeof(b)-1,"VERSUS: RACES %u  WINS %u",r61_profile.vs_races,r61_profile.vs_wins);
            R61_SNPRINTF(c,sizeof(c)-1,"BATTLE: MATCHES %u  WINS %u",r61_profile.battle_matches,r61_profile.battle_wins);
            R61_SNPRINTF(d,sizeof(d)-1,"%s: PROFILE INFO  / UNRANKED STATS",R61_STATS_LABEL);
        }else{
            R61_SNPRINTF(a,sizeof(a)-1,"%s  %s / %s",r61_profile.playing?"IN GAME":r61_profile.online?"ONLINE":"OFFLINE",r61_profile.platform,r61_profile.region);
            R61_SNPRINTF(b,sizeof(b)-1,"JOINED: %s  FRIENDS: %u",r61_profile.joined,r61_profile.friends);
            if(self)R61_SNPRINTF(c,sizeof(c)-1,"MAIL: %u UNREAD  REQUESTS: %u",r61_profile.unread,r61_profile.requests);
            else R61_SNPRINTF(c,sizeof(c)-1,"FRIEND STATUS: %s",r61_profile.relation);
            R61_SNPRINTF(d,sizeof(d)-1,"%s: GP / VERSUS / BATTLE STATS",R61_STATS_LABEL);
        }
        if(self)R61_SNPRINTF(footer,sizeof(footer)-1,"A MAILBOX  X REQUESTS  Y FRIENDS  B BACK");
        else R61_SNPRINTF(footer,sizeof(footer)-1,r61_match(r61_profile.relation,"FRIEND")?"Y REMOVE FRIEND  X MESSAGE  B BACK":"A %s  X MESSAGE  B BACK",r61_match(r61_profile.relation,"INCOMING")?"ACCEPT FRIEND":r61_match(r61_profile.relation,"OUTGOING")?"REQUEST SENT":"ADD FRIEND");
        if(notice[0])r61_copy(d,sizeof(d),notice);
        title[sizeof(title)-1]=a[sizeof(a)-1]=b[sizeof(b)-1]=c[sizeof(c)-1]=d[sizeof(d)-1]=footer[sizeof(footer)-1]=0;
        R61_SCREEN(title,a,b,c,d,footer);
        r61_modal_service();unsigned q=R61_BUTTONS();if(q&R61_B)break;
        if(q&R61_STATS)page=1-page;else if(q&R61_RIGHT)page=1;else if(q&R61_LEFT)page=0;
        if(self){if(q&R61_A){r61_mailbox_view();r61_fetch_profile(wanted);R61_CONSUME();}else if(q&R61_X){r61_request_view();r61_fetch_profile(wanted);R61_CONSUME();}else if(q&R61_Y){r61_friends_view();r61_fetch_profile(wanted);R61_CONSUME();}}
        else if(q&R61_A){
            if(!r61_match(r61_profile.relation,"FRIEND")&&!r61_match(r61_profile.relation,"OUTGOING")){
                bool ok=r61_friend_action(wanted,r61_match(r61_profile.relation,"INCOMING")?"ACCEPT":"REQUEST");
                r61_copy(notice,sizeof(notice),ok?"FRIEND ACTION CONFIRMED":r61_result_code[0]?r61_result_code:"FRIEND ACTION TIMED OUT");r61_fetch_profile(wanted);
            }
            R61_CONSUME();
        }else if(!self&&(q&R61_X)){
            bool ok=r61_mail_compose(wanted);r61_copy(notice,sizeof(notice),ok?"PRIVATE MESSAGE SENT":r61_result_code[0]?r61_result_code:"MESSAGE NOT SENT");R61_CONSUME();
        }else if(!self&&(q&R61_Y)&&r61_match(r61_profile.relation,"FRIEND")){
            R61_CONSUME();bool confirm=false;
            for(;;){R61_SCREEN("REMOVE FRIEND?",r61_profile.name,"THIS REMOVES BOTH FRIEND LINKS","","","A CONFIRM  B CANCEL");
                r61_modal_service();unsigned choice=R61_BUTTONS();if(choice&R61_B)break;if(choice&R61_A){confirm=true;break;}Sleep(16);}
            if(confirm){bool ok=r61_friend_action(wanted,"REMOVE");r61_copy(notice,sizeof(notice),ok?"FRIEND REMOVED":"REMOVE FAILED");r61_fetch_profile(wanted);}R61_CONSUME();
        }
        Sleep(16);
    }
    R61_WORLD_SUPPRESS=old;R61_CONSUME();
}
static void r61_request_view(){
    bool old=R61_WORLD_SUPPRESS;R61_WORLD_SUPPRESS=true;R61_CONSUME();
    r61_request_offset=0;if(!r61_fetch_requests()){R61_WORLD_SUPPRESS=old;R61_CONSUME();return;}
    int row=0;
    for(;;){
        char a[72]={0},b[72]={0},c[72]={0},d[72]={0},title[72];char *out[4]={a,b,c,d};
        R61_SNPRINTF(title,sizeof(title)-1,"REQUESTS (%d-%d / %d)",r61_request_total?r61_request_offset+1:0,r61_request_offset+r61_count_requests(),r61_request_total);
        if(!r61_request_total)r61_copy(a,sizeof(a),"NO PENDING FRIEND REQUESTS");
        else for(int j=0;j<4;++j){int i=j+(row>=4?1:0);if(i>=5)break;if(!r61_requests[i].found)continue;R61_SNPRINTF(out[j],71,"%c %s",row==i?'>':' ',r61_requests[i].name);out[j][71]=0;}
        R61_SCREEN(title,a,b,c,d,"A VIEW / ACCEPT  B BACK");
        r61_modal_service();unsigned q=R61_BUTTONS();if(q&R61_B)break;
        int count=r61_count_requests();
        if((q&R61_UP)&&count){if(row>0)--row;else if(r61_request_offset>0){r61_request_offset-=5;r61_fetch_requests();row=4;}else row=count-1;}
        if((q&R61_DOWN)&&count){if(row+1<count)++row;else if(r61_request_offset+5<r61_request_total){r61_request_offset+=5;r61_fetch_requests();row=0;}else{r61_request_offset=0;r61_fetch_requests();row=0;}}
        if((q&R61_A)&&count&&r61_requests[row].found){char id[20];r61_copy(id,sizeof(id),r61_requests[row].id);r61_profile_view(id);r61_request_offset=0;r61_fetch_requests();row=0;R61_CONSUME();}
        Sleep(16);
    }
    R61_WORLD_SUPPRESS=old;R61_CONSUME();
}
static void r61_friends_view(){
    bool old=R61_WORLD_SUPPRESS;R61_WORLD_SUPPRESS=true;R61_CONSUME();
    r61_friend_offset=0;if(!r61_fetch_friends()){R61_WORLD_SUPPRESS=old;R61_CONSUME();return;}
    int row=0;
    for(;;){
        char title[72],a[80]={0},b[80]={0},c[80]={0},d[80]={0};char *lines[4]={a,b,c,d};
        int count=r61_count_friends();R61_SNPRINTF(title,sizeof(title)-1,"FRIENDS (%d-%d / %d)",r61_friend_total?r61_friend_offset+1:0,r61_friend_offset+count,r61_friend_total);
        if(!count)r61_copy(a,sizeof(a),"NO FRIENDS ADDED YET");
        else for(int j=0;j<4;++j){int i=j+(row>=4?1:0);if(i>=5||!r61_friends[i].found)continue;R61_SNPRINTF(lines[j],79,"%c %s",row==i?'>':' ',r61_friends[i].name);lines[j][79]=0;}
        R61_SCREEN(title,a,b,c,d,"A VIEW PROFILE  UP/DOWN  B BACK");
        r61_modal_service();unsigned q=R61_BUTTONS();if(q&R61_B)break;
        if((q&R61_UP)&&count){if(row>0)--row;else if(r61_friend_offset>0){r61_friend_offset-=5;r61_fetch_friends();row=4;}else row=count-1;}
        if((q&R61_DOWN)&&count){if(row+1<count)++row;else if(r61_friend_offset+5<r61_friend_total){r61_friend_offset+=5;r61_fetch_friends();row=0;}else{r61_friend_offset=0;r61_fetch_friends();row=0;}}
        if((q&R61_A)&&count&&r61_friends[row].found){char id[20];r61_copy(id,sizeof(id),r61_friends[row].id);r61_profile_view(id);r61_fetch_friends();row=0;R61_CONSUME();}
        Sleep(16);
    }
    R61_WORLD_SUPPRESS=old;R61_CONSUME();
}
static void r61_mailbox_view(){
    bool old=R61_WORLD_SUPPRESS;R61_WORLD_SUPPRESS=true;R61_CONSUME();
    r61_mail_offset=0;if(!r61_fetch_mail()){R61_WORLD_SUPPRESS=old;R61_CONSUME();return;}
    int row=0;
    for(;;){
        char title[72],a[96]={0},b[96]={0},c[96]={0},d[96]={0};char *lines[4]={a,b,c,d};
        R61_SNPRINTF(title,sizeof(title)-1,"MAILBOX %d NEW (%d-%d / %d)",r61_mail_unread,r61_mail_total?r61_mail_offset+1:0,r61_mail_offset+r61_count_mail(),r61_mail_total);
        int count=r61_count_mail();if(!count)r61_copy(a,sizeof(a),"NO PRIVATE MESSAGES YET");
        else for(int j=0;j<4;++j){int index=count>4&&row>=4?j+1:j;if(index>=5||!r61_mail[index].found)continue;R61_SNPRINTF(lines[j],95,"%c %s %s",index==row?'>':' ',r61_mail[index].unread?"NEW":"   ",r61_mail[index].name);lines[j][95]=0;}
        R61_SCREEN(title,a,b,c,d,"A READ  Y FRIEND REQUESTS  B BACK");
        r61_modal_service();unsigned q=R61_BUTTONS();if(q&R61_B)break;
        if((q&R61_UP)&&count){if(row>0)--row;else if(r61_mail_offset>0){r61_mail_offset-=5;r61_fetch_mail();row=4;}else row=count-1;}
        if((q&R61_DOWN)&&count){if(row+1<count)++row;else if(r61_mail_offset+5<r61_mail_total){r61_mail_offset+=5;r61_fetch_mail();row=0;}else{r61_mail_offset=0;r61_fetch_mail();row=0;}}
        if(q&R61_Y){r61_request_view();r61_mail_offset=0;r61_fetch_mail();row=0;R61_CONSUME();}
        if((q&R61_A)&&count&&r61_mail[row].found){
            R61Mail selected=r61_mail[row];char header[56],ftr[80];R61_SNPRINTF(header,sizeof(header)-1,"FROM: %s",selected.name);R61_SNPRINTF(ftr,sizeof(ftr)-1,"X REPLY  B BACK");
            char req[180];R61_SNPRINTF(req,sizeof(req)-1,"MKDIR2|SOCIAL_MAIL_READ|%s|%s|%s",r48_me()->id,r48_me()->secret,selected.id);req[sizeof(req)-1]=0;
            r61_wait(req,"READ");R61_CONSUME();
            for(;;){R61_SCREEN("PRIVATE MESSAGE",header,selected.body,"","",ftr);r61_modal_service();unsigned p=R61_BUTTONS();if(p&R61_B)break;if(p&R61_X){r61_mail_compose(selected.from);R61_CONSUME();}Sleep(16);}
            r61_fetch_mail();row=0;R61_CONSUME();
        }
        Sleep(16);
    }
    R61_WORLD_SUPPRESS=old;R61_CONSUME();
}
static void r61_menu_draw(int left_row,bool focus_right,int right_row);
#endif
