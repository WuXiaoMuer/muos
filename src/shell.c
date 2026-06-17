#include "shell.h"
#include "vga.h"
#include "keyboard.h"
#include "mouse.h"
#include "pit.h"
#include "mm.h"
#include "task.h"

#define CMD_MAX  128
#define HIST_MAX 16
#define HIST_LEN 128
#define MAX_ARGS 16
#define MAX_WIN  6

static char cmd_buf[CMD_MAX]; static int cmd_pos, cmd_len;
static char history[HIST_MAX][HIST_LEN]; static int hist_count, hist_index;

typedef struct { int x,y,w,h,id; const char*title; uint8_t bc; } Win;

static int str_eq(const char*a,const char*b){while(*a&&*b){if(*a!=*b)return 0;a++;b++;}return(*a==*b);}
static int str_starts(const char*s,const char*p){while(*p){if(*s!=*p)return 0;s++;p++;}return 1;}
static void str_cpy(char*d,const char*s){while(*s)*d++=*s++;*d=0;}
static int str_len(const char*s){int n=0;while(*s++)n++;return n;}

static void show_prompt(void){vga_setcolor(vga_entry_color(VGA_LIGHT_GREEN,VGA_BLACK));vga_print("muos> ");vga_setcolor(vga_entry_color(VGA_LIGHT_GREY,VGA_BLACK));}
static void redraw_line(void){uint16_t r=vga_get_cursor_row();vga_set_cursor(r,6);for(int i=0;i<cmd_len+1;i++)vga_putchar(' ');vga_set_cursor(r,6);for(int i=0;i<cmd_len;i++)vga_putchar(cmd_buf[i]);vga_set_cursor(r,6+cmd_pos);}
static void ins_char(char c){if(cmd_len>=CMD_MAX-1)return;for(int i=cmd_len;i>cmd_pos;i--)cmd_buf[i]=cmd_buf[i-1];cmd_buf[cmd_pos++]=c;cmd_len++;redraw_line();}
static void del_char(void){if(cmd_pos<cmd_len){for(int i=cmd_pos;i<cmd_len-1;i++)cmd_buf[i]=cmd_buf[i+1];cmd_len--;redraw_line();}}
static void bksp_char(void){if(cmd_pos>0){for(int i=cmd_pos-1;i<cmd_len-1;i++)cmd_buf[i]=cmd_buf[i+1];cmd_pos--;cmd_len--;redraw_line();}}
static void hist_add(const char*l){if(!l[0])return;if(hist_count>0&&str_eq(history[hist_count-1],l))return;if(hist_count<HIST_MAX)str_cpy(history[hist_count++],l);else{for(int i=0;i<HIST_MAX-1;i++)str_cpy(history[i],history[i+1]);str_cpy(history[HIST_MAX-1],l);}}
static void hist_recall(int d){if(!hist_count)return;int i=hist_index+d;if(i<0)i=0;if(i>=hist_count)i=hist_count;hist_index=i;if(i<hist_count){str_cpy(cmd_buf,history[i]);cmd_len=str_len(cmd_buf);cmd_pos=cmd_len;redraw_line();}}
static const char*cmds[]={"help","clear","cls","echo","mem","tasks","time","ps","reboot","halt","crash","logo","version","gui",NULL};
static void tab_complete(void){if(!cmd_pos)return;int ws=0;for(int i=cmd_pos-1;i>=0;i--){if(cmd_buf[i]==' '){ws=i+1;break;}}int wl=cmd_pos-ws;if(!wl)return;int m=0;const char*match=NULL;for(int i=0;cmds[i];i++){if(str_starts(cmds[i],cmd_buf+ws)){m++;match=cmds[i];}}if(m==1&&match){cmd_len=ws;cmd_pos=ws;for(const char*p=match;*p;p++)ins_char(*p);ins_char(' ');}}
static void shell_readline(void){cmd_len=0;cmd_pos=0;cmd_buf[0]=0;hist_index=hist_count;show_prompt();for(;;){char c=keyboard_getchar();switch((unsigned char)c){case'\n':case'\r':cmd_buf[cmd_len]=0;vga_putchar('\n');return;case'\b':bksp_char();break;case'\t':tab_complete();break;case KEY_LEFT:if(cmd_pos>0){cmd_pos--;vga_move_cursor_left();}break;case KEY_RIGHT:if(cmd_pos<cmd_len){cmd_pos++;vga_move_cursor_right();}break;case KEY_HOME:cmd_pos=0;vga_set_cursor(vga_get_cursor_row(),6);break;case KEY_END:cmd_pos=cmd_len;vga_set_cursor(vga_get_cursor_row(),6+cmd_len);break;case KEY_DELETE:del_char();break;case KEY_UP:hist_recall(-1);break;case KEY_DOWN:hist_recall(1);break;default:if(c>=' '&&c<127)ins_char(c);break;}}}
static int tokenize(char*b,char**a){int n=0;char*p=b;while(*p){while(*p==' ')p++;if(!*p)break;a[n++]=p;while(*p&&*p!=' ')p++;if(*p)*p++=0;}a[n]=NULL;return n;}

static void cmd_help(void){vga_print("\n  help clear echo mem tasks time\n  ps logo version gui reboot halt crash\n\n");}
static void cmd_clear(void){vga_clear();}
static void cmd_echo(char**a,int n){for(int i=1;i<n;i++){vga_print(a[i]);if(i<n-1)vga_putchar(' ');}vga_putchar('\n');}
static void cmd_mem(void){vga_print("Memory: ");vga_print_dec(mm_get_total_pages()*4);vga_print("KB total, ");vga_print_dec(mm_get_free_pages()*4);vga_print("KB free\n");}
static void cmd_tasks(void){vga_print("Tasks: ");vga_print_dec(task_get_count());vga_putchar('\n');task_list();}
static void cmd_time(void){uint32_t s=pit_get_ticks()/100;vga_print("Uptime: ");vga_print_dec(s/3600);vga_putchar('h');vga_print_dec((s/60)%60);vga_putchar('m');vga_print_dec(s%60);vga_putchar('s');vga_print(" (");vga_print_dec(pit_get_ticks());vga_print(" ticks)\n");}
static void cmd_ps(void){task_t*t=task_get_current();if(t){vga_print("Current: ");vga_print(t->name);vga_print(" pid=");vga_print_dec(t->pid);vga_putchar('\n');}task_list();}
static void cmd_logo(void){vga_setcolor(vga_entry_color(VGA_LIGHT_CYAN,VGA_BLACK));vga_print("\n  __  __       ___  ____\n |  \\/  |_   _/ _ \\/ ___|\n | |\\/| | | | | | | \\___ \\\n | |  | | |_| | |_| |___) |\n |_|  |_|\\__,_|\\___/|____/\n\n");vga_setcolor(vga_entry_color(VGA_LIGHT_GREY,VGA_BLACK));}
static void cmd_version(void){vga_setcolor(vga_entry_color(VGA_LIGHT_CYAN,VGA_BLACK));vga_print("MuOS v0.3\nBuilt: "__DATE__" "__TIME__"\nGCC 15.2.0\n");vga_setcolor(vga_entry_color(VGA_LIGHT_GREY,VGA_BLACK));}
static void cmd_reboot(void){vga_print("Rebooting...\n");for(volatile int i=0;i<5000000;i++)__asm__ volatile("nop");uint8_t z[6]={0};__asm__ volatile("lidt %0"::"m"(z));__asm__ volatile("int $0");}
static void cmd_halt(void){vga_print("Halted.\n");__asm__ volatile("cli; hlt");}
static void cmd_crash(void){vga_setcolor(vga_entry_color(VGA_LIGHT_RED,VGA_BLACK));vga_print("\n!!! PANIC !!!\n\n");vga_setcolor(vga_entry_color(VGA_LIGHT_GREY,VGA_BLACK));*(volatile uint32_t*)0=0xDEADBEEF;}

/* ═══════════════════════════════════════ TUI Desktop ═══════════════════════════════════════ */
static Win wins[MAX_WIN]; static int nwin,active;

static void fd(int y,int x1,int x2,uint8_t c,char ch){uint16_t v=vga_entry(ch,c);for(int x=x1;x<=x2;x++)((volatile uint16_t*)0xB8000)[y*80+x]=v;}
static void fdt(int x,int y,const char*s,uint8_t c){while(*s){((volatile uint16_t*)0xB8000)[y*80+x]=vga_entry(*s,c);x++;s++;}}
static void nts(char*b,uint32_t n){int p=0;if(!n)b[p++]='0';else{int d[10],c=0;while(n){d[c++]=n%10;n/=10;}while(c)b[p++]='0'+d[--c];}b[p]=0;}

static void draw_frame(Win*w,int focus){
    uint16_t* vm=(uint16_t*)0xB8000;
    uint8_t tc=focus?vga_entry_color(VGA_WHITE,VGA_YELLOW):vga_entry_color(VGA_BLACK,VGA_LIGHT_GREY);
    uint8_t bc=w->bc;
    int x=w->x,y=w->y,ww=w->w,hh=w->h;
    fd(y,x+1,x+ww-2,tc,' '); fdt(x+2,y,w->title,tc);
    vm[y*80+x]=vga_entry(0xDA,tc); vm[y*80+x+ww-1]=vga_entry(0xBF,tc);
    for(int r=y+1;r<y+hh;r++){vm[r*80+x]=vga_entry(0xB3,bc);vm[r*80+x+ww-1]=vga_entry(0xB3,bc);fd(r,x+1,x+ww-2,bc,' ');}
    int by=y+hh;fd(by,x+1,x+ww-2,bc,0xC4);vm[by*80+x]=vga_entry(0xC0,bc);vm[by*80+x+ww-1]=vga_entry(0xD9,bc);
    for(int r=y+1;r<=by;r++)vm[r*80+x+ww]=vga_entry(0xB0,vga_entry_color(VGA_BLACK,VGA_BLACK));
    fd(by+1,x+1,x+ww+1,vga_entry_color(VGA_BLACK,VGA_BLACK),0xB0);
}

static void draw_desk_bg(void){
    vga_clear();
    fd(0,0,79,vga_entry_color(VGA_WHITE,VGA_BLUE),' ');
    fdt(3,0,"  M u O S   D e s k t o p   v 0 . 3",vga_entry_color(VGA_WHITE,VGA_BLUE));
    fd(24,0,79,vga_entry_color(VGA_BLACK,VGA_LIGHT_GREY),' ');
    fdt(1,24," TAB:Focus  Arrows:Move  DEL:Close  F1-F6:Win  ESC:Quit",vga_entry_color(VGA_BLACK,VGA_LIGHT_GREY));
}

static void draw_content(Win*w){
    switch(w->id){
    case 1: /* Terminal */
        fdt(w->x+2,w->y+2,"$ muos> _",vga_entry_color(VGA_LIGHT_GREEN,VGA_BLACK));
        fdt(w->x+2,w->y+4,"Commands:",vga_entry_color(VGA_YELLOW,VGA_BLACK));
        fdt(w->x+2,w->y+5,"help clear echo mem",vga_entry_color(VGA_LIGHT_GREY,VGA_BLACK));
        fdt(w->x+2,w->y+6,"tasks time ps logo",vga_entry_color(VGA_LIGHT_GREY,VGA_BLACK));
        fdt(w->x+2,w->y+7,"version reboot halt",vga_entry_color(VGA_LIGHT_GREY,VGA_BLACK));
        break;
    case 2: /* Memory */
        {uint32_t tot=mm_get_total_pages()*4,fr=mm_get_free_pages()*4,us=tot-fr;char b[16];
        fdt(w->x+2,w->y+2,"Total: ",vga_entry_color(VGA_GREEN,VGA_BLACK));nts(b,tot);fdt(w->x+10,w->y+2,b,vga_entry_color(VGA_GREEN,VGA_BLACK));fdt(w->x+16,w->y+2,"KB",vga_entry_color(VGA_GREEN,VGA_BLACK));
        fdt(w->x+2,w->y+3,"Used:  ",vga_entry_color(VGA_LIGHT_RED,VGA_BLACK));nts(b,us);fdt(w->x+10,w->y+3,b,vga_entry_color(VGA_LIGHT_RED,VGA_BLACK));fdt(w->x+16,w->y+3,"KB",vga_entry_color(VGA_LIGHT_RED,VGA_BLACK));
        fdt(w->x+2,w->y+4,"Free:  ",vga_entry_color(VGA_LIGHT_GREEN,VGA_BLACK));nts(b,fr);fdt(w->x+10,w->y+4,b,vga_entry_color(VGA_LIGHT_GREEN,VGA_BLACK));fdt(w->x+16,w->y+4,"KB",vga_entry_color(VGA_LIGHT_GREEN,VGA_BLACK));
        int bar=28,f=tot?(int)((uint32_t)us*bar/tot):0;
        fdt(w->x+2,w->y+5,"[",vga_entry_color(VGA_GREEN,VGA_BLACK));
        for(int i=0;i<f;i++)fdt(w->x+3+i,w->y+5,"=",vga_entry_color(VGA_LIGHT_RED,VGA_BLACK));
        for(int i=f;i<bar;i++)fdt(w->x+3+i,w->y+5,"-",vga_entry_color(VGA_DARK_GREY,VGA_BLACK));
        fdt(w->x+3+bar,w->y+5,"]",vga_entry_color(VGA_GREEN,VGA_BLACK));
        fdt(w->x+2,w->y+7,"Pages: ",vga_entry_color(VGA_GREEN,VGA_BLACK));nts(b,mm_get_total_pages());fdt(w->x+9,w->y+7,b,vga_entry_color(VGA_GREEN,VGA_BLACK));}
        break;
    case 3: /* Tasks */
        fdt(w->x+2,w->y+1,"PID  Name     State",vga_entry_color(VGA_YELLOW,VGA_BLACK));
        fdt(w->x+2,w->y+2,"---  -------- -----",vga_entry_color(VGA_YELLOW,VGA_BLACK));
        {task_t*t=task_get_current();if(t){int y=w->y+3;task_t*cur=t;do{char b[32];int p=0;nts(&b[p],cur->pid);p=str_len(b);while(p<6)b[p++]=' ';for(const char*s=cur->name;*s;s++)b[p++]=*s;while(p<17)b[p++]=' ';const char*st=cur->state==1?"RUN":cur->state==2?"BLK":"RDY";for(const char*s=st;*s;s++)b[p++]=*s;b[p]=0;fdt(w->x+2,y++,b,vga_entry_color(VGA_CYAN,VGA_BLACK));cur=cur->next;}while(cur&&cur!=t&&y<w->y+w->h-1);}}
        break;
    case 4: /* Help */
        fdt(w->x+2,w->y+2,"TAB   : Switch window",vga_entry_color(VGA_BLACK,VGA_LIGHT_GREY));
        fdt(w->x+2,w->y+3,"Arrows: Move window",vga_entry_color(VGA_BLACK,VGA_LIGHT_GREY));
        fdt(w->x+2,w->y+4,"DEL   : Close window",vga_entry_color(VGA_BLACK,VGA_LIGHT_GREY));
        fdt(w->x+2,w->y+5,"ESC   : Exit desktop",vga_entry_color(VGA_BLACK,VGA_LIGHT_GREY));
        fdt(w->x+2,w->y+6,"F1-F6 : Select window",vga_entry_color(VGA_BLACK,VGA_LIGHT_GREY));
        break;
    case 5: /* Logo */
        fdt(w->x+2,w->y+2," __  __       ___  ____",vga_entry_color(VGA_WHITE,VGA_BLUE));
        fdt(w->x+2,w->y+3,"|  \\/  |_   _/ _ \\/ ___|",vga_entry_color(VGA_WHITE,VGA_BLUE));
        fdt(w->x+2,w->y+4,"| |\\/| | | | | | | \\___ \\",vga_entry_color(VGA_WHITE,VGA_BLUE));
        fdt(w->x+2,w->y+5,"| |  | | |_| | |_| |___) |",vga_entry_color(VGA_WHITE,VGA_BLUE));
        fdt(w->x+2,w->y+6,"|_|  |_|\\__,_|\\___/|____/",vga_entry_color(VGA_WHITE,VGA_BLUE));
        break;
    }
}

static void add_win(int x,int y,int w,int h,int id,const char*t,uint8_t bc){if(nwin>=MAX_WIN)return;wins[nwin].x=x;wins[nwin].y=y;wins[nwin].w=w;wins[nwin].h=h;wins[nwin].id=id;wins[nwin].title=t;wins[nwin].bc=bc;active=nwin;nwin++;}
static void move_win(Win*w,int dx,int dy){w->x+=dx;w->y+=dy;if(w->x<1)w->x=1;if(w->y<1)w->y=1;if(w->x+w->w>79)w->x=79-w->w;if(w->y+w->h>23)w->y=23-w->h;}

static void cmd_gui(void){
    keyboard_flush(); nwin=0; active=-1;
    add_win(2,3,38,10,1," Terminal ",vga_entry_color(VGA_LIGHT_GREY,VGA_BLACK));
    add_win(42,3,36,12,2," Memory Monitor ",vga_entry_color(VGA_GREEN,VGA_BLACK));
    add_win(2,14,38,8,3," Task Manager ",vga_entry_color(VGA_CYAN,VGA_BLACK));
    add_win(42,16,36,8,4," Help ",vga_entry_color(VGA_BLACK,VGA_LIGHT_GREY));
    add_win(20,1,40,10,5," MuOS Logo ",vga_entry_color(VGA_WHITE,VGA_BLUE));
    draw_desk_bg();
    for(int i=0;i<nwin;i++){draw_frame(&wins[i],i==active);draw_content(&wins[i]);}
    int mdown=0,msel=-1;

    while(1){
        /* clock */
        {uint32_t s=pit_get_ticks()/100;char b[8];nts(b,s);fdt(65,0,b,vga_entry_color(VGA_LIGHT_GREEN,VGA_BLUE));fdt(65+str_len(b),0,"s",vga_entry_color(VGA_LIGHT_GREEN,VGA_BLUE));}
        /* mouse */
        mouse_state_t ms=mouse_get_state();
        int mx8=ms.x/8, my16=ms.y/16;
        if(mx8>=0&&mx8<80&&my16>=0&&my16<25){
            /* show cursor as inverted char at current position */
            uint16_t* vm=(uint16_t*)0xB8000; uint16_t orig=vm[my16*80+mx8];
            vm[my16*80+mx8]=vga_entry((orig&0xFF)?(orig&0xFF):' ',(orig>>8)^0x77);
            /* mouse click handling */
            if(ms.btn_left&&!mdown){ mdown=1; msel=-1;
                for(int i=nwin-1;i>=0;i--){Win*w=&wins[i];
                    if(mx8>=w->x&&mx8<w->x+w->w&&my16>=w->y&&my16<w->y+w->h){
                        msel=i; active=i; break;
                    }}
                if(msel>=0){draw_desk_bg();for(int i=0;i<nwin;i++){draw_frame(&wins[i],i==active);draw_content(&wins[i]);}}
            }
            if(!ms.btn_left) mdown=0;
        }
        /* keyboard */
        if(keyboard_haschar()){
            char c=keyboard_getchar();
            switch((unsigned char)c){
            case KEY_ESC: vga_clear(); return;
            case '\t': if(nwin>0){active=(active+1)%nwin;draw_desk_bg();for(int i=0;i<nwin;i++){draw_frame(&wins[i],i==active);draw_content(&wins[i]);}} break;
            case KEY_UP:    if(active>=0){move_win(&wins[active],0,-1);draw_desk_bg();for(int i=0;i<nwin;i++){draw_frame(&wins[i],i==active);draw_content(&wins[i]);}} break;
            case KEY_DOWN:  if(active>=0){move_win(&wins[active],0,1);draw_desk_bg();for(int i=0;i<nwin;i++){draw_frame(&wins[i],i==active);draw_content(&wins[i]);}} break;
            case KEY_LEFT:  if(active>=0){move_win(&wins[active],-1,0);draw_desk_bg();for(int i=0;i<nwin;i++){draw_frame(&wins[i],i==active);draw_content(&wins[i]);}} break;
            case KEY_RIGHT: if(active>=0){move_win(&wins[active],1,0);draw_desk_bg();for(int i=0;i<nwin;i++){draw_frame(&wins[i],i==active);draw_content(&wins[i]);}} break;
            case KEY_DELETE: if(active>=0&&nwin>1){for(int i=active;i<nwin-1;i++)wins[i]=wins[i+1];nwin--;if(active>=nwin)active=nwin-1;draw_desk_bg();for(int i=0;i<nwin;i++){draw_frame(&wins[i],i==active);draw_content(&wins[i]);}} break;
            case KEY_F1:case KEY_F2:case KEY_F3:case KEY_F4:case KEY_F5:{int id=c-KEY_F1;if(id<nwin){active=id;draw_desk_bg();for(int i=0;i<nwin;i++){draw_frame(&wins[i],i==active);draw_content(&wins[i]);}}}break;
            }
        } else {
            __asm__ volatile ("hlt");
        }
    }
}

static void shell_execute(void){
    if(!cmd_len)return;
    hist_add(cmd_buf);
    char*av[MAX_ARGS];int ac=tokenize(cmd_buf,av);if(!ac)return;
    char*cmd=av[0];
    if(str_eq(cmd,"help"))cmd_help();
    else if(str_eq(cmd,"clear")||str_eq(cmd,"cls"))cmd_clear();
    else if(str_eq(cmd,"echo"))cmd_echo(av,ac);
    else if(str_eq(cmd,"mem"))cmd_mem();
    else if(str_eq(cmd,"tasks"))cmd_tasks();
    else if(str_eq(cmd,"time"))cmd_time();
    else if(str_eq(cmd,"ps"))cmd_ps();
    else if(str_eq(cmd,"logo"))cmd_logo();
    else if(str_eq(cmd,"version"))cmd_version();
    else if(str_eq(cmd,"gui")||str_eq(cmd,"desktop"))cmd_gui();
    else if(str_eq(cmd,"reboot"))cmd_reboot();
    else if(str_eq(cmd,"halt"))cmd_halt();
    else if(str_eq(cmd,"crash"))cmd_crash();
    else{vga_setcolor(vga_entry_color(VGA_LIGHT_RED,VGA_BLACK));vga_print("Unknown: ");vga_print(cmd);vga_putchar('\n');vga_setcolor(vga_entry_color(VGA_LIGHT_GREY,VGA_BLACK));}
}

void shell_run(void){
    vga_setcolor(vga_entry_color(VGA_YELLOW,VGA_BLACK));vga_print("\n  MuOS Shell v0.3\n\n");
    vga_setcolor(vga_entry_color(VGA_LIGHT_GREY,VGA_BLACK));
    for(;;){shell_readline();shell_execute();}
}
