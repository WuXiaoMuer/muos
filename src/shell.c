/* shell.c - MuOS Shell v0.5 */
#include "shell.h"
#include "vga.h"
#include "keyboard.h"
#include "pit.h"
#include "mm.h"
#include "fs.h"
#include "editor.h"
#include "win7.h"
#include "test.h"
#include "task.h"
#include "string.h"
#include "logo.h"

/* ── Types ────────────────────────────── */
#define CMD_MAX   128
#define HIST_MAX  16
#define HIST_LEN  128
#define MAX_ARGS  16

/* ── Line Editor State ───────────────── */
static char cmd_buf[CMD_MAX];
static int  cmd_pos, cmd_len;
static char history[HIST_MAX][HIST_LEN];
static int  hist_count, hist_index;

/* ── String Helpers ──────────────────── */
/* strcmp/strcpy/strlen/u32_to_dec come from string.c */
static int streq(const char*a,const char*b){return strcmp(a,b)==0;}

/* ── Line Editor (forward-declared before tab) ── */
static void prompt(void){vga_setcolor(vga_entry_color(VGA_LIGHT_GREEN,VGA_BLACK));vga_print("muos> ");vga_setcolor(vga_entry_color(VGA_LIGHT_GREY,VGA_BLACK));}
static void redraw_line(void){
    uint16_t r=vga_get_cursor_row();vga_set_cursor(r,6);
    for(int i=0;i<cmd_len+1;i++)vga_putchar(' ');
    vga_set_cursor(r,6);
    for(int i=0;i<cmd_len;i++)vga_putchar(cmd_buf[i]);
    vga_set_cursor(r,6+cmd_pos);
}
static void isrt(char c){
    if(cmd_len>=CMD_MAX-1)return;
    for(int i=cmd_len;i>cmd_pos;i--)cmd_buf[i]=cmd_buf[i-1];
    cmd_buf[cmd_pos++]=c;cmd_len++;redraw_line();
}

/* ── Tab Completion ──────────────────── */
static const char*cmds[]={"help","clear","cls","echo","mem","tasks","time","ps","reboot","halt","crash","logo","version","gui","win7","touch","ls","cat","rm","write","edit","calc","test",NULL};
static void tab(void){
    if(!cmd_pos)return;
    int ws=0;
    for(int i=cmd_pos-1;i>=0;i--){if(cmd_buf[i]==' '){ws=i+1;break;}}
    int wl=cmd_pos-ws; if(!wl)return;
    int m=0; const char*mt=NULL;
    for(int i=0;cmds[i];i++) if(strncmp(cmd_buf+ws,cmds[i],strlen(cmds[i]))==0){m++;mt=cmds[i];}
    if(m==1&&mt){cmd_len=ws;cmd_pos=ws;for(const char*p=mt;*p;p++)isrt(*p);isrt(' ');}
}
static void bksp(void){
    if(!cmd_pos)return;
    for(int i=cmd_pos-1;i<cmd_len-1;i++)cmd_buf[i]=cmd_buf[i+1];
    cmd_pos--;cmd_len--;redraw_line();
}
static void h_add(const char*l){
    if(!*l)return;
    if(hist_count>0&&streq(history[hist_count-1],l))return;
    if(hist_count<HIST_MAX)strcpy(history[hist_count++],l);
    else{for(int i=0;i<HIST_MAX-1;i++)strcpy(history[i],history[i+1]);strcpy(history[HIST_MAX-1],l);}
}
static void h_recall(int d){
    if(!hist_count)return;
    int i=hist_index+d;
    if(i<0)i=0;
    if(i>=hist_count)i=hist_count;
    hist_index=i;
    if(i<hist_count){strcpy(cmd_buf,history[i]);cmd_len=(int)strlen(cmd_buf);cmd_pos=cmd_len;redraw_line();}
}
static void readline(void){
    cmd_len=0;cmd_pos=0;cmd_buf[0]=0;hist_index=hist_count;prompt();
    for(;;){char c=keyboard_getchar();switch((unsigned char)c){
    case'\n':case'\r':cmd_buf[cmd_len]=0;vga_putchar('\n');return;
    case'\b':bksp();break;
    case'\t':tab();break;
    case KEY_LEFT:if(cmd_pos){cmd_pos--;vga_move_cursor_left();}break;
    case KEY_RIGHT:if(cmd_pos<cmd_len){cmd_pos++;vga_move_cursor_right();}break;
    case KEY_HOME:cmd_pos=0;vga_set_cursor(vga_get_cursor_row(),6);break;
    case KEY_END: cmd_pos=cmd_len;vga_set_cursor(vga_get_cursor_row(),6+cmd_len);break;
    case KEY_DELETE:{if(cmd_pos<cmd_len){for(int i=cmd_pos;i<cmd_len-1;i++)cmd_buf[i]=cmd_buf[i+1];cmd_len--;redraw_line();}}break;
    case KEY_UP:h_recall(-1);break;
    case KEY_DOWN:h_recall(1);break;
    default:if(c>=' '&&c<127)isrt(c);break;
    }}
}
static int tokenize(char*b,char**a,int max_args){int n=0;char*p=b;while(*p){while(*p==' ')p++;if(!*p)break;if(n>=max_args-1)break;a[n++]=p;while(*p&&*p!=' ')p++;if(*p)*p++=0;}a[n]=NULL;return n;}

/* ═══════════════════ Commands ═══════════════════ */
static void cmd_help(void){
    vga_print("  touch ls cat rm write edit  - filesystem\n");
    vga_print("  help clear echo mem tasks time ps logo\n");
    vga_print("  version gui win7 test reboot halt crash\n");
}
static void cmd_test(void){ if(tests_run()) vga_print("SELF-TEST FAILED\n"); }
static void cmd_clear(void){vga_clear();}
static void cmd_echo(char**a,int n){for(int i=1;i<n;i++){vga_print(a[i]);if(i<n-1)vga_putchar(' ');}vga_putchar('\n');}
static void cmd_mem(void){vga_print("Mem: ");vga_print_dec(mm_get_total_pages()*4);vga_print("KB tot ");vga_print_dec(mm_get_free_pages()*4);vga_print("KB free\n");}
static void cmd_tasks(void){vga_print("Tasks: ");vga_print_dec(task_get_count());vga_putchar('\n');task_list();}
static void cmd_time(void){uint32_t s=pit_get_ticks()/100;
    vga_print("Up ");vga_print_dec(s/3600);vga_putchar('h');
    vga_print_dec((s/60)%60);vga_putchar('m');vga_print_dec(s%60);vga_putchar('s');
    vga_print(" (");vga_print_dec(pit_get_ticks());vga_print("t)\n");
}
static void cmd_ps(void){task_t*t=task_get_current();if(t){vga_print(t->name);vga_print(" pid=");vga_print_dec(t->pid);vga_putchar('\n');}task_list();}
static void cmd_logo(void){
    vga_setcolor(vga_entry_color(VGA_LIGHT_CYAN,VGA_BLACK));
    vga_print("\n");
    for (int i = 0; i < 5; i++) {
        vga_print(muos_logo[i]);
        vga_putchar('\n');
    }
    vga_print("\n");
    vga_setcolor(vga_entry_color(VGA_LIGHT_GREY,VGA_BLACK));
}
static void cmd_version(void){vga_setcolor(vga_entry_color(VGA_LIGHT_CYAN,VGA_BLACK));vga_print("MuOS v0.3\n"__DATE__" "__TIME__"\n");vga_setcolor(vga_entry_color(VGA_LIGHT_GREY,VGA_BLACK));}
static void cmd_reboot(void){vga_print("Reboot...\n");for(volatile int i=0;i<5000000;i++)__asm__ volatile("nop");uint8_t z[6]={0};__asm__ volatile("lidt %0"::"m"(z));__asm__ volatile("int $0");}
static void cmd_halt(void){vga_print("Halted.\n");__asm__ volatile("cli;hlt");}
static void cmd_crash(void){vga_setcolor(vga_entry_color(VGA_LIGHT_RED,VGA_BLACK));vga_print("\nPANIC!\n\n");vga_setcolor(vga_entry_color(VGA_LIGHT_GREY,VGA_BLACK));*(volatile uint32_t*)0=0xDEADBEEF;}

/* ── FS Commands ──────────────────────── */
static void cmd_touch(char**a,int n){if(n<2){vga_print("touch <name>\n");return;}int f=fs_create(a[1]);if(f>=0){vga_print("OK: ");vga_print(a[1]);vga_putchar('\n');}else{vga_setcolor(vga_entry_color(VGA_LIGHT_RED,VGA_BLACK));vga_print("Fail\n");vga_setcolor(vga_entry_color(VGA_LIGHT_GREY,VGA_BLACK));}}
static void cmd_ls(void){int n=fs_count();vga_print_dec(n);vga_print(" file(s)\n");for(int i=0;i<n;i++){vga_print("  ");vga_print(fs_name(i));vga_print("  ");vga_print_dec(fs_size(i));vga_print("B\n");}}
static void cmd_cat(char**a,int n){if(n<2){vga_print("cat <file>\n");return;}int f=fs_open(a[1]);if(f<0){vga_print("Not found\n");return;}char b[1024];int l=fs_read(f,b,1023);b[l]=0;vga_print(b);vga_putchar('\n');}

/* ── Calculator ──────────────────────── */
static void cmd_calc(char**a,int n){
    if(n<2){vga_print("calc <expr>\n  e.g. calc 2+3  calc 10*5  calc 100/4\n");return;}
    /* Simple single-op calculator: num op num */
    /* Parse argv[1] as "N+M" "N-M" "N*M" "N/M" */
    const char*e=a[1];
    uint32_t num1=0,num2=0;char op=0;int i=0;
    while(e[i]>='0'&&e[i]<='9'){num1=num1*10+(e[i]-'0');i++;}
    if(e[i]=='+'||e[i]=='-'||e[i]=='*'||e[i]=='/'){op=e[i];i++;}
    while(e[i]>='0'&&e[i]<='9'){num2=num2*10+(e[i]-'0');i++;}
    uint32_t result=0;
    switch(op){
    case'+':result=num1+num2;break;
    case'-':result=num1-num2;break;
    case'*':result=num1*num2;break;
    case'/':result=num2?num1/num2:0;break;
    default:vga_print("Bad expr. Use: N+M N-M N*M N/M\n");return;
    }
    vga_print_dec(num1);vga_putchar(op);vga_print_dec(num2);vga_print(" = ");vga_print_dec(result);vga_putchar('\n');
}
static void cmd_rm(char**a,int n){if(n<2){vga_print("rm <file>\n");return;}if(!fs_delete(a[1])){vga_print("Deleted: ");vga_print(a[1]);vga_putchar('\n');}else vga_print("Not found\n");}
static void cmd_write(char**a,int n){if(n<3){vga_print("write <file> <text>\n");return;}int f=fs_open(a[1]);if(f<0)f=fs_create(a[1]);if(f<0){vga_print("Fail\n");return;}char b[1024];int p=0;for(int i=2;i<n&&p<1023;i++){for(const char*s=a[i];*s&&p<1023;s++)b[p++]=*s;if(i<n-1&&p<1023)b[p++]=' ';}b[p]=0;fs_write(f,b,p);vga_print("Wrote ");vga_print_dec(p);vga_print("B to ");vga_print(a[1]);vga_putchar('\n');}

/* ── Execute ─────────────────────────── */
static void execute(void){
    if(!cmd_len)return;
    h_add(cmd_buf);
    char*av[MAX_ARGS]; int ac=tokenize(cmd_buf,av,MAX_ARGS); if(!ac)return;
    char*cmd=av[0];
    if(streq(cmd,"help"))cmd_help();
    else if(streq(cmd,"clear")||streq(cmd,"cls"))cmd_clear();
    else if(streq(cmd,"echo"))cmd_echo(av,ac);
    else if(streq(cmd,"mem"))cmd_mem();
    else if(streq(cmd,"tasks"))cmd_tasks();
    else if(streq(cmd,"time"))cmd_time();
    else if(streq(cmd,"ps"))cmd_ps();
    else if(streq(cmd,"logo"))cmd_logo();
    else if(streq(cmd,"version"))cmd_version();
    else if(streq(cmd,"gui")||streq(cmd,"desktop"))win7_run();
    else if(streq(cmd,"win7"))win7_run();
    else if(streq(cmd,"reboot"))cmd_reboot();
    else if(streq(cmd,"halt"))cmd_halt();
    else if(streq(cmd,"crash"))cmd_crash();
    else if(streq(cmd,"touch"))cmd_touch(av,ac);
    else if(streq(cmd,"ls"))cmd_ls();
    else if(streq(cmd,"cat"))cmd_cat(av,ac);
    else if(streq(cmd,"rm"))cmd_rm(av,ac);
    else if(streq(cmd,"write"))cmd_write(av,ac);
    else if(streq(cmd,"edit")){editor_run(ac>1?av[1]:NULL);vga_clear();}
    else if(streq(cmd,"calc"))cmd_calc(av,ac);
    else if(streq(cmd,"test"))cmd_test();
}

void shell_run(void){
    fs_init();
    fs_create("readme.txt");
    fs_write(fs_open("readme.txt"), "Welcome to MuOS 7!\nType 'help' for commands.\nType 'win7' for desktop.", 69);
    fs_create("notes.txt");
    fs_write(fs_open("notes.txt"), "MuOS 7 - x86 Microkernel\nBuilt with GCC 15 + NASM", 49);

    vga_setcolor(vga_entry_color(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print("\n");
    for (int i = 0; i < 5; i++) {
        vga_print(muos_logo[i]);
        vga_putchar('\n');
    }
    vga_print("\n");
    vga_setcolor(vga_entry_color(VGA_YELLOW, VGA_BLACK));
    vga_print("  MuOS 7 Ultimate  |  256MB  |  ");
    vga_print(__DATE__);
    vga_print("\n  Type 'help' or 'win7'\n\n");
    vga_setcolor(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
    for(;;){readline();execute();}
}
