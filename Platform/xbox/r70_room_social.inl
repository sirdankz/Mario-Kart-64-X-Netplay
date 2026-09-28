/* Copyright (c) 2026 Sirdankz
 * SPDX-License-Identifier: MPL-2.0
 * R70 control-plane only. Include after R61 and R62 in the netplay translation unit.
 * The room roster and the unranked wins display never enter gameplay hashes. */
#ifndef R70_ROOM_SOCIAL_INL
#define R70_ROOM_SOCIAL_INL
struct R70Member {bool found;char id[20],name[20],platform[16];};
struct R70Top {bool found;char id[20],name[20];unsigned total,gp,vs,battle;};

/* R72 leaderboards: all replies are tagged by category/course to avoid stale
 * UDP responses being attributed to the next page. Pure control-plane state. */
struct R72Entry {bool found;char id[20],name[20];unsigned value;};
static R72Entry r72_rows[5];
static unsigned r72_total=0;
static bool r72_done=false,r72_ended=false;
static char r72_category[9]={0};
static int r72_course=-1;
static int r72_count(){int n=0;for(int i=0;i<5;++i)if(r72_rows[i].found)++n;return n;}

static R70Member r70_members[4];
static R70Top r70_top[5];
static unsigned r70_members_total=0,r70_top_total=0;
static bool r70_members_done=false,r70_top_done=false,r70_members_end=false,r70_top_end=false;
static int r70_member_count(){int n=0;for(int i=0;i<4;++i)if(r70_members[i].found)++n;return n;}
static int r70_top_count(){int n=0;for(int i=0;i<5;++i)if(r70_top[i].found)++n;return n;}
static bool r70_social_packet(const char *b){
    if(!b||strncmp(b,"MKDIR2|SOCIAL_",14))return false;
    unsigned n=0,index=0;
    if(sscanf(b,"MKDIR2|SOCIAL_ROOMBEGIN|%u",&n)==1){
        if(n>4)n=4;
        r70_members_total=n;memset(r70_members,0,sizeof(r70_members));r70_members_done=false;r70_members_end=false;
        return true;
    }
    if(!strncmp(b,"MKDIR2|SOCIAL_ROOM_USER|",24)){
        R70Member m;memset(&m,0,sizeof(m));
        if(sscanf(b,"MKDIR2|SOCIAL_ROOM_USER|%u|%19[^|]|%19[^|]|%15[^\r\n]",&index,m.id,m.name,m.platform)==4 && index<4){
            m.found=true;r70_members[index]=m;
            if(r70_members_end && (unsigned)r70_member_count()>=r70_members_total)r70_members_done=true;
        }
        return true;
    }
    if(sscanf(b,"MKDIR2|SOCIAL_ROOMEND|%u",&n)==1){
        if(n<=4 && n==r70_members_total){r70_members_end=true;r70_members_done=(unsigned)r70_member_count()>=n;}
        return true;
    }
    if(sscanf(b,"MKDIR2|SOCIAL_TOPBEGIN|%u",&n)==1){
        if(n>5)n=5;
        r70_top_total=n;memset(r70_top,0,sizeof(r70_top));r70_top_done=false;r70_top_end=false;
        return true;
    }
    if(!strncmp(b,"MKDIR2|SOCIAL_TOP_USER|",23)){
        R70Top m;memset(&m,0,sizeof(m));
        if(sscanf(b,"MKDIR2|SOCIAL_TOP_USER|%u|%19[^|]|%19[^|]|%u|%u|%u|%u",
                  &index,m.id,m.name,&m.total,&m.gp,&m.vs,&m.battle)==7 && index<5){
            m.found=true;r70_top[index]=m;
            if(r70_top_end && (unsigned)r70_top_count()>=r70_top_total)r70_top_done=true;
        }
        return true;
    }
    if(sscanf(b,"MKDIR2|SOCIAL_TOPEND|%u",&n)==1){
        if(n<=5 && n==r70_top_total){r70_top_end=true;r70_top_done=(unsigned)r70_top_count()>=n;}
        return true;
    }
    /* R72 reply modes are independent of the legacy R70 combined Top 5. */
    char cat[9]={0};int course=-1;
    if(sscanf(b,"MKDIR2|SOCIAL_LB_BEGIN|%8[A-Z]|%u",cat,&n)==2){
        if(!strcmp(cat,r72_category)&&r72_course<0&&n<=5){
            r72_total=n;r72_done=r72_ended=false;memset(r72_rows,0,sizeof(r72_rows));
        }return true;
    }
    if(!strncmp(b,"MKDIR2|SOCIAL_LB_USER|",22)){
        R72Entry e;memset(&e,0,sizeof(e));
        if(sscanf(b,"MKDIR2|SOCIAL_LB_USER|%8[A-Z]|%u|%19[^|]|%19[^|]|%u",
                  cat,&index,e.id,e.name,&e.value)==5&&index<5&&
           !strcmp(cat,r72_category)&&r72_course<0){
            e.found=true;r72_rows[index]=e;
            if(r72_ended&&(unsigned)r72_count()>=r72_total)r72_done=true;
        }return true;
    }
    if(sscanf(b,"MKDIR2|SOCIAL_LB_END|%8[A-Z]|%u",cat,&n)==2){
        if(!strcmp(cat,r72_category)&&r72_course<0&&n==r72_total){
            r72_ended=true;r72_done=(unsigned)r72_count()>=n;
        }return true;
    }
    if(sscanf(b,"MKDIR2|SOCIAL_TT_BEGIN|%d|%u",&course,&n)==2){
        if(course==r72_course&&n<=5){
            r72_total=n;r72_done=r72_ended=false;memset(r72_rows,0,sizeof(r72_rows));
        }return true;
    }
    if(!strncmp(b,"MKDIR2|SOCIAL_TT_USER|",22)){
        R72Entry e;memset(&e,0,sizeof(e));
        if(sscanf(b,"MKDIR2|SOCIAL_TT_USER|%d|%u|%19[^|]|%19[^|]|%u",
                  &course,&index,e.id,e.name,&e.value)==5&&index<5&&course==r72_course){
            e.found=true;r72_rows[index]=e;
            if(r72_ended&&(unsigned)r72_count()>=r72_total)r72_done=true;
        }return true;
    }
    if(sscanf(b,"MKDIR2|SOCIAL_TT_END|%d|%u",&course,&n)==2){
        if(course==r72_course&&n==r72_total){
            r72_ended=true;r72_done=(unsigned)r72_count()>=n;
        }return true;
    }
    return false;
}
/* Keep the Hub's non-gameplay UDP control socket serviced during overlays. */
static bool r70_wait(const char *q,bool for_room){
    DWORD begin=GetTickCount(),last=0;
    while(GetTickCount()-begin<2400U){
        /* A host may start the race while a guest is browsing. Abort rather
         * than holding the simulation in a blocking directory wait. */
        if(for_room && !R70_LOBBY_RUNNING)return false;
        DWORD now=GetTickCount();
        if(!last||now-last>=650U){r48_send(q);last=now;}
        r48_drain();r61_modal_service();
        if(for_room?r70_members_done:r70_top_done)return true;
        Sleep(10);
    }
    return false;
}
static bool r70_fetch_room(){
    if(!r48_me()||!R70_ROOM_ID[0])return false;
    r70_members_done=r70_members_end=false;r70_members_total=0;memset(r70_members,0,sizeof(r70_members));
    char q[190];R61_SNPRINTF(q,sizeof(q)-1,"MKDIR2|SOCIAL_ROOM_PLAYERS|%s|%s|%s",
                             r48_me()->id,r48_me()->secret,R70_ROOM_ID);q[sizeof(q)-1]=0;
    return r70_wait(q,true);
}
static bool r70_fetch_top(){
    if(!r48_me())return false;
    r70_top_done=r70_top_end=false;r70_top_total=0;memset(r70_top,0,sizeof(r70_top));
    char q[160];R61_SNPRINTF(q,sizeof(q)-1,"MKDIR2|SOCIAL_TOP_WINS|%s|%s",
                             r48_me()->id,r48_me()->secret);q[sizeof(q)-1]=0;
    return r70_wait(q,false);
}
static bool r70_lobby_running(){return R70_LOBBY_RUNNING;}
/* D-Pad RIGHT from either public host or guest lobby; A opens the same
 * authenticated profile screen used by the Public Match online roster. */
static void r70_room_view(){
    bool old=R61_WORLD_SUPPRESS;R61_WORLD_SUPPRESS=true;R61_CONSUME();
    if(!r70_fetch_room()){
        if(!r70_lobby_running()){R61_WORLD_SUPPRESS=old;R61_CONSUME();return;}
        R61_SCREEN("ROOM PLAYERS UNAVAILABLE","HUB R2.16 IS REQUIRED","OR THE ROOM IS NO LONGER ACTIVE","","","B BACK");
        Sleep(850);R61_WORLD_SUPPRESS=old;R61_CONSUME();return;
    }
    int row=0;
    while(r70_lobby_running()){
        char lines[4][80]={{0}},title[72];
        if(!r70_members_total)r61_copy(lines[0],sizeof(lines[0]),"NO MEMBERS IN THIS ROOM");
        for(unsigned i=0;i<r70_members_total&&i<4;++i){
            if(!r70_members[i].found)continue;
            R61_SNPRINTF(lines[i],79,"%c %.18s  [%s]",row==(int)i?'>':' ',r70_members[i].name,r70_members[i].platform);
            lines[i][79]=0;
        }
        R61_SNPRINTF(title,sizeof(title)-1,"ROOM PLAYERS (%u)",r70_members_total);title[sizeof(title)-1]=0;
        R61_SCREEN(title,lines[0],lines[1],lines[2],lines[3],"A PROFILE / FRIEND  X REFRESH  B BACK");
        r61_modal_service();unsigned q=R61_BUTTONS();
        if(q&R61_B)break;
        if((q&R61_UP)&&r70_members_total)row=(row+(int)r70_members_total-1)%(int)r70_members_total;
        if((q&R61_DOWN)&&r70_members_total)row=(row+1)%(int)r70_members_total;
        if(q&R61_X){r70_fetch_room();row=0;R61_CONSUME();continue;}
        if((q&R61_A)&&r70_members_total&&r70_members[row].found){
            char id[20];r61_copy(id,sizeof(id),r70_members[row].id);
            r61_profile_view(id,false);R61_CONSUME();
            if(!r70_lobby_running())break;
            r70_fetch_room();row=0;
        }
        Sleep(16);
    }
    R61_WORLD_SUPPRESS=old;R61_CONSUME();
}
/* MK64_R72_CATEGORY_LEADERBOARDS_AND_TT_COURSES */
static const int r72_tt_ids[16]={8,9,6,11,10,5,1,0,14,12,7,2,18,4,3,13};
static const char *r72_tt_names[16]={"LUIGI RACEWAY","MOO MOO FARM","KOOPA TROOPA BEACH","KALIMARI DESERT",
    "TOADS TURNPIKE","FRAPPE SNOWLAND","CHOCO MOUNTAIN","MARIO RACEWAY",
    "WARIO STADIUM","SHERBET LAND","ROYAL RACEWAY","BOWSERS CASTLE",
    "DK JUNGLE PARKWAY","YOSHI VALLEY","BANSHEE BOARDWALK","RAINBOW ROAD"};
static bool r72_fetch(const char *cat,int course){
    if(!r48_me())return false;
    r72_category[0]=0;if(cat)r61_copy(r72_category,sizeof(r72_category),cat);
    r72_course=course;r72_total=0;r72_done=r72_ended=false;memset(r72_rows,0,sizeof(r72_rows));
    char q[180];
    if(course>=0)R61_SNPRINTF(q,sizeof(q)-1,"MKDIR2|SOCIAL_TT_TOP|%s|%s|%d",
                              r48_me()->id,r48_me()->secret,course);
    else R61_SNPRINTF(q,sizeof(q)-1,"MKDIR2|SOCIAL_LEADERBOARD|%s|%s|%s",
                       r48_me()->id,r48_me()->secret,r72_category);
    q[sizeof(q)-1]=0;
    DWORD begin=GetTickCount(),last=0;
    while(GetTickCount()-begin<2500U){
        DWORD now=GetTickCount();if(!last||now-last>=650U){r48_send(q);last=now;}
        r48_drain();r61_modal_service();if(r72_done)return true;Sleep(10);
    }
    return false;
}
static void r72_show_board(const char *cat,int course,const char *title){
    bool ok=r72_fetch(cat,course);int row=0;
    for(;;){
        char lines[4][96]={{0}};unsigned count=ok?r72_total:0;
        if(!ok)r61_copy(lines[0],sizeof(lines[0]),"HUB LEADERBOARD UNAVAILABLE");
        else if(!count){
            r61_copy(lines[0],sizeof(lines[0]),course>=0?"NO RECORDED TIMES YET":"NO CONFIRMED WINS YET");
            if(course>=0)r61_copy(lines[1],sizeof(lines[1]),"PLAY SOLO ONLINE TO SAVE TIMES");
        }
        int first=row>=4?1:0;
        for(int i=0;i<4;++i){int j=i+first;if(j>=(int)count||!r72_rows[j].found)continue;
            if(course>=0){unsigned ms=r72_rows[j].value;
                R61_SNPRINTF(lines[i],95,"%c%d. %.15s %u:%02u.%03u",j==row?'>':' ',j+1,
                             r72_rows[j].name,ms/60000U,(ms/1000U)%60U,ms%1000U);
            }else R61_SNPRINTF(lines[i],95,"%c%d. %.17s %u WINS",j==row?'>':' ',j+1,
                                r72_rows[j].name,r72_rows[j].value);
            lines[i][95]=0;
        }
        R61_SCREEN(title,lines[0],lines[1],lines[2],lines[3],course>=0?"SELF-REPORTED  A PROFILE X REFRESH B BACK":"PEER-CONFIRMED  A PROFILE X REFRESH B BACK");
        r61_modal_service();unsigned q=R61_BUTTONS();
        if(q&R61_B)break;
        if((q&R61_UP)&&count)row=(row+(int)count-1)%(int)count;
        if((q&R61_DOWN)&&count)row=(row+1)%(int)count;
        if(q&R61_X){ok=r72_fetch(cat,course);row=0;R61_CONSUME();continue;}
        if((q&R61_A)&&count&&r72_rows[row].found){
            char id[20];r61_copy(id,sizeof(id),r72_rows[row].id);
            r61_profile_view(id,true);R61_CONSUME();ok=r72_fetch(cat,course);row=0;
        }
        Sleep(16);
    }
    R61_CONSUME();
}
static void r72_tt_menu(){
    int row=0;R61_CONSUME();
    for(;;){
        char lines[4][96]={{0}};
        int first=row-1;if(first<0)first=0;if(first>12)first=12;
        for(int i=0;i<4;++i){int j=first+i;
            R61_SNPRINTF(lines[i],95,"%c %s",j==row?'>':' ',r72_tt_names[j]);
            lines[i][95]=0;
        }
        char pageInfo[72];R61_SNPRINTF(pageInfo,sizeof(pageInfo)-1,
            "TRACKS %02d-%02d/16  UP/DOWN SCROLL  A TOP 5  B BACK",first+1,first+4);
        pageInfo[sizeof(pageInfo)-1]=0;
        R61_SCREEN("TIME TRIALS - SELECT COURSE",lines[0],lines[1],lines[2],lines[3],pageInfo);
        r61_modal_service();unsigned q=R61_BUTTONS();if(q&R61_B)break;
        if(q&R61_UP)row=(row+15)%16;if(q&R61_DOWN)row=(row+1)%16;
        if(q&R61_A){r72_show_board(0,r72_tt_ids[row],r72_tt_names[row]);R61_CONSUME();}
        Sleep(16);
    }R61_CONSUME();
}
/* Existing Public Match row calls this function. */
static void r70_top_view(){
    bool old=R61_WORLD_SUPPRESS;R61_WORLD_SUPPRESS=true;R61_CONSUME();
    const char *names[4]={"TOP 5 GP WINS","TOP 5 VERSUS WINS","TOP 5 BATTLE WINS","TOP TIME TRIALS"};
    const char *categories[3]={"GP","VS","BATTLE"};
    int row=0;
    for(;;){
        char lines[4][80]={{0}};
        for(int i=0;i<4;++i){R61_SNPRINTF(lines[i],79,"%c %s",i==row?'>':' ',names[i]);lines[i][79]=0;}
        R61_SCREEN("LEADERBOARDS",lines[0],lines[1],lines[2],lines[3],"A SELECT  B BACK");
        r61_modal_service();unsigned q=R61_BUTTONS();if(q&R61_B)break;
        if(q&R61_UP)row=(row+3)%4;if(q&R61_DOWN)row=(row+1)%4;
        if(q&R61_A){if(row==3)r72_tt_menu();else r72_show_board(categories[row],-1,names[row]);R61_CONSUME();}
        Sleep(16);
    }R61_WORLD_SUPPRESS=old;R61_CONSUME();
}
#endif
