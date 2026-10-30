/* shell.c - MuOS Shell v0.5 */
#include "shell.h"
#include "vga.h"
#include "keyboard.h"
#include "pit.h"
#include "mm.h"
#include "fs.h"
#include "editor.h"
#include "win7.h"
#include "gui.h"
#include "test.h"
#include "task.h"
#include "string.h"
#include "logo.h"
#include "io.h"
#include "serial.h"
#include "user.h"
#include "module.h"
#include "ata.h"
#include "part.h"
#include "fat.h"
#include "ext2.h"

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
static const char*cmds[]={"help","clear","cls","echo","mem","tasks","time","ps","reboot","halt","crash","logo","version","gui","win7","desktop","touch","ls","cat","rm","write","edit","calc","test","run","hello","exec","mods","execmod","pixel","disk","part","fatls","fatcat","ext2ls","ext2cat",NULL};
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
/* Split the command line into argv, honouring single/double quotes.
 * Quotes are removed; spaces inside quotes are preserved. */
static int tokenize(char*b,char**a,int max_args){
    int n=0; char*p=b;
    while(*p){
        while(*p==' ')p++;
        if(!*p)break;
        if(n>=max_args-1)break;
        char*out=p; a[n++]=out; char quote=0;
        while(*p){
            if(quote){
                if(*p==quote){quote=0;p++;continue;}
                *out++=*p++;
            } else if(*p=='"'||*p=='\''){
                quote=*p++;
            } else if(*p==' '){
                break;
            } else {
                *out++=*p++;
            }
        }
        if(*p==' ')p++;
        *out='\0';
    }
    a[n]=NULL; return n;
}

/* ═══════════════════ Commands ═══════════════════ */
static void cmd_help(void){
    vga_print("  touch ls cat rm write edit  - filesystem\n");
    vga_print("  run [fault|pie|dyn|tls]     - Ring3 C program (Linux ABI)\n");
    vga_print("  exec <file>                 - run an ELF from the RAM FS\n");
    vga_print("  mods / execmod <n>          - run a Multiboot -initrd module\n");
    vga_print("  help clear echo mem tasks time ps disk logo\n");
    vga_print("  version gui win7 desktop pixel test reboot halt crash\n");
    vga_print("  part fatls [p] fatcat <f> ext2ls [p] ext2cat <f>\n");
}
static void cmd_test(void){ if(tests_run()) vga_print("SELF-TEST FAILED\n"); }
static void slog_u32(uint32_t v);
static void cmd_disk(void){
    if(!ata_present()){ vga_print("No ATA disk on the primary bus\n"); return; }
    vga_print(" /dev/hda: ");
    vga_print(ata_model());
    vga_print("\n  sectors="); vga_print_dec(ata_sector_count());
    vga_print(" ("); vga_print_dec(ata_sector_count()/2); vga_print(" KB)\n");
    if(fs_persist_active()){
        vga_print("  MuFS mounted, generation=");
        vga_print_dec(fs_persist_generation()); vga_putchar('\n');
        serial_write("[disk] MuFS gen=");
        slog_u32(fs_persist_generation());
        serial_write("\n");
    }
    serial_write("[disk] "); serial_write(ata_model()); serial_write("\n");
}

/* ---- partition table / FAT32 / ext2 (read-only) ---- */
static char  fs_out[2048];
static uint8_t fs_file[1024];

static void cmd_part(void){
    if(!ata_present()){ vga_print("No ATA disk\n"); return; }
    if(!part_is_partitioned()){
        vga_print("No partition table (whole-disk volume)\n");
        serial_write("[part] none\n");
        return;
    }
    vga_print(" partitions="); vga_print_dec(part_count()); vga_putchar('\n');
    for(int i=0;i<part_count();i++){
        part_t p;
        if(!part_get(i,&p)) continue;
        vga_print("  ["); vga_print_dec(i); vga_print("] start=");
        vga_print_dec((uint32_t)p.start_lba);
        vga_print(" sectors="); vga_print_dec((uint32_t)p.sectors);
        vga_print(p.gpt?" GPT":" MBR");
        vga_print(" type=0x"); vga_print_hex(p.type);
        if(p.name[0]){ vga_print(" name="); vga_print(p.name); }
        vga_putchar('\n');
    }
    serial_write("[part] table ok\n");
}

static void cmd_fatls(char**a,int n){
    if(!fat_present()){ vga_print("No FAT32 volume\n"); serial_write("[fat] absent\n"); return; }
    const char* p = (n>1)?a[1]:"/";
    int r = fat_list(p, fs_out, sizeof(fs_out));
    if(r < 0){ vga_print("FAT: cannot list path\n"); serial_write("[fat] ls fail\n"); return; }
    vga_print(fs_out);
    serial_write(fs_out);
    serial_write("[fat] ls ok\n");
}

static void cmd_fatcat(char**a,int n){
    if(!fat_present()){ vga_print("No FAT32 volume\n"); serial_write("[fat] absent\n"); return; }
    if(n<2){ vga_print("usage: fatcat <file>\n"); return; }
    int r = fat_read_file(a[1], fs_file, sizeof(fs_file)-1);
    if(r < 0){ vga_print("FAT: cannot read file\n"); serial_write("[fat] cat fail\n"); return; }
    fs_file[r] = 0;
    vga_print("[fat] "); vga_print(a[1]); vga_print(": ");
    vga_print_dec((uint32_t)r); vga_print(" bytes\n");
    vga_print((const char*)fs_file);
    serial_write((const char*)fs_file);
    serial_write("[fat] cat ok\n");
}

static void cmd_ext2ls(char**a,int n){
    if(!ext2_present()){ vga_print("No ext2 volume\n"); serial_write("[ext2] absent\n"); return; }
    const char* p = (n>1)?a[1]:"/";
    int r = ext2_list(p, fs_out, sizeof(fs_out));
    if(r < 0){ vga_print("ext2: cannot list path\n"); serial_write("[ext2] ls fail\n"); return; }
    vga_print(fs_out);
    serial_write(fs_out);
    serial_write("[ext2] ls ok\n");
}

static void cmd_ext2cat(char**a,int n){
    if(!ext2_present()){ vga_print("No ext2 volume\n"); serial_write("[ext2] absent\n"); return; }
    if(n<2){ vga_print("usage: ext2cat <file>\n"); return; }
    int r = ext2_read_file(a[1], fs_file, sizeof(fs_file)-1);
    if(r < 0){ vga_print("ext2: cannot read file\n"); serial_write("[ext2] cat fail\n"); return; }
    fs_file[r] = 0;
    vga_print("[ext2] "); vga_print(a[1]); vga_print(": ");
    vga_print_dec((uint32_t)r); vga_print(" bytes\n");
    vga_print((const char*)fs_file);
    serial_write((const char*)fs_file);
    serial_write("[ext2] cat ok\n");
}

static void cmd_run(char**a,int n){
    /* Runs the embedded ELF at Ring3; returns when the process exits or
     * faults. `run fault` asks hello.c to touch an unmapped address, which
     * demonstrates that a Ring3 fault kills only the process. `run pie`
     * uses the ET_DYN (PIE) build so the loader applies relocations.
     * `run dyn` installs /lib/ld-mu.so + /lib/libmu.so and runs the
     * dynamically linked program (PT_INTERP + DT_NEEDED). */
    if(n>1 && streq(a[1],"pie"))
        user_run_pie_embedded("hello-pie", "pie");
    else if(n>1 && streq(a[1],"dyn"))
        user_run_dyn_embedded();
    else if(n>1 && streq(a[1],"tls"))
        user_run_tls_embedded();
    else
        user_run_embedded("hello", n>1?a[1]:NULL);
    vga_setcolor(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
}
static void slog_u32(uint32_t v){ char b[12]; u32_to_dec(b,v); serial_write(b); }
static void cmd_mods(void){
    uint32_t n=modules_count();
    vga_print_dec(n); vga_print(" module(s)\n");
    serial_write("[mods] count="); slog_u32(n); serial_write("\n");
    for(uint32_t i=0;i<n;i++){
        uint32_t s=0,e=0;
        modules_get(i,&s,&e);
        serial_write("[mods] "); slog_u32(i);
        serial_write(" start="); slog_u32(s);
        serial_write(" end="); slog_u32(e);
        serial_write(" size="); slog_u32(e-s); serial_write("\n");
        vga_print("  ["); vga_print_dec(i); vga_print("] ");
        vga_print_hex(s); vga_print("-"); vga_print_hex(e);
        vga_print("  "); vga_print_dec(e-s); vga_print("B  ");
        const char* nm=modules_name(i);
        vga_print(nm&&nm[0]?nm:"(unnamed)");
        vga_putchar('\n');
    }
}
static void cmd_execmod(char**a,int n){
    uint32_t idx=0;
    if(n>1){ for(const char* p=a[1]; *p>='0'&&*p<='9'; p++) idx=idx*10+(uint32_t)(*p-'0'); }
    uint32_t s=0,e=0;
    if(!modules_get(idx,&s,&e)){ vga_print("No such module\n"); return; }
    uint32_t size=e-s;
    if(size==0){ vga_print("Empty module\n"); return; }

    /* The ELF may live inside the user sandbox address range; user_run_image
     * clears the sandbox, so copy it to kernel pages first. */
    uint32_t pages=(size+PAGE_SIZE-1)/PAGE_SIZE;
    void* copy=mm_alloc_pages(pages);
    if(!copy){ vga_print("Not enough contiguous memory for module copy\n"); return; }
    uint32_t cs=(uint32_t)copy, ce=cs+size;
    if(cs < USER_END && ce > USER_BASE){
        mm_free_pages(copy,pages);
        vga_print("Module copy landed inside the user sandbox; retry\n");
        return;
    }
    memcpy(copy,(const void*)s,size);

    serial_write("[execmod] start="); slog_u32(s);
    serial_write(" size="); slog_u32(size);
    serial_write(" magic=");
    if(memcmp(copy,"\x7f""ELF",4)==0) serial_write("ELF\n"); else serial_write("BAD\n");

    const char* nm=modules_name(idx);
    const char* base=nm&&nm[0]?nm:"module";
    for(const char* p=base; *p; p++){
        if(*p=='/'||*p=='\\') base=p+1;   /* argv[0] must be a bare name */
    }

    const char* argv[16];
    int argc=1;
    argv[0]=base[0]?base:"module";
    for(int i=2;i<n && argc<16;i++) argv[argc++]=a[i];
    user_run_image_argv(copy,size,argc,argv);
    mm_free_pages(copy,pages);
    vga_setcolor(vga_entry_color(VGA_LIGHT_GREY,VGA_BLACK));
}
static void cmd_exec(char**a,int n){
    if(n<2){vga_print("exec <file>\n");return;}
    int slot=fs_open(a[1]);
    if(slot<0){vga_print("Not found\n");return;}
    uint32_t sz=fs_fd_size(slot);
    if(!sz){vga_print("Empty file\n");return;}
    user_run_image(fs_fd_data(slot), sz, a[1], n>2?a[2]:NULL);
    vga_setcolor(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
}
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
static void cmd_version(void){vga_setcolor(vga_entry_color(VGA_LIGHT_CYAN,VGA_BLACK));vga_print("MuOS v1.2 (Ring3 + Linux ABI)\n"__DATE__" "__TIME__"\n");vga_setcolor(vga_entry_color(VGA_LIGHT_GREY,VGA_BLACK));}
static void cmd_reboot(void){
    vga_print("Reboot...\n");
    serial_write("[shell] reboot requested (8042 reset line)\n");
    for(volatile int i=0;i<1000000;i++)__asm__ volatile("nop");
    __asm__ volatile("cli");
    /* Pulse the 8042 CPU-reset line (A20 gate port, 0xFE = pulse reset).
     * Reliable on QEMU/real hardware; the old lidt+int$0 triple-fault
     * trick did not even reliably reset. */
    outb(0x64, 0xFE);
    for(volatile int i=0;i<2000000;i++)__asm__ volatile("nop");
    /* Fallback if the controller is unresponsive: zeroed IDT -> triple fault */
    serial_write("[shell] 8042 reset failed, forcing triple fault\n");
    uint8_t z[6]={0};
    __asm__ volatile("lidt %0"::"m"(z));
    __asm__ volatile("int $0");
}
static void cmd_halt(void){vga_print("Halted.\n");serial_write("[shell] halt\n");__asm__ volatile("cli;hlt");}
static void cmd_crash(void){
    /* The identity map covers all RAM, so a null write no longer faults.
     * 0xE0000000 lies above ram_top with no page table -> guaranteed
     * Page Fault (CR2 = 0xE0000000) for the panic-path demo. */
    vga_setcolor(vga_entry_color(VGA_LIGHT_RED,VGA_BLACK));
    vga_print("\nPANIC!\n\n");
    vga_setcolor(vga_entry_color(VGA_LIGHT_GREY,VGA_BLACK));
    serial_write("[shell] crash: writing to unmapped 0xE0000000\n");
    *(volatile uint32_t*)0xE0000000 = 0xDEADBEEF;
}

/* ── FS Commands ──────────────────────── */
static void cmd_touch(char**a,int n){if(n<2){vga_print("touch <name>\n");return;}int f=fs_create(a[1]);if(f>=0){vga_print("OK: ");vga_print(a[1]);vga_putchar('\n');}else{vga_setcolor(vga_entry_color(VGA_LIGHT_RED,VGA_BLACK));vga_print("Fail\n");vga_setcolor(vga_entry_color(VGA_LIGHT_GREY,VGA_BLACK));}}
static void cmd_ls(void){int n=fs_count();vga_print_dec(n);vga_print(" file(s)\n");for(int i=0;i<n;i++){vga_print("  ");vga_print(fs_name(i));vga_print("  ");vga_print_dec(fs_size(i));vga_print("B\n");}}
static void cmd_cat(char**a,int n){
    if(n<2){vga_print("cat <file>\n");return;}
    int f=fs_open(a[1]);if(f<0){vga_print("Not found\n");return;}
    const char* d=fs_fd_data(f);
    uint32_t sz=fs_fd_size(f);
    for(uint32_t i=0;i<sz;i++) vga_putchar(d[i]);
    if(sz==0||d[sz-1]!='\n') vga_putchar('\n');
}

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
    serial_write("[cmd] "); serial_write(cmd_buf); serial_write("\n");
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
    else if(streq(cmd,"pixel"))gui_run();
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
    else if(streq(cmd,"run")||streq(cmd,"hello"))cmd_run(av,ac);
    else if(streq(cmd,"exec"))cmd_exec(av,ac);
    else if(streq(cmd,"mods"))cmd_mods();
    else if(streq(cmd,"disk"))cmd_disk();
    else if(streq(cmd,"part"))cmd_part();
    else if(streq(cmd,"fatls"))cmd_fatls(av,ac);
    else if(streq(cmd,"fatcat"))cmd_fatcat(av,ac);
    else if(streq(cmd,"ext2ls"))cmd_ext2ls(av,ac);
    else if(streq(cmd,"ext2cat"))cmd_ext2cat(av,ac);
    else if(streq(cmd,"execmod"))cmd_execmod(av,ac);
}

void shell_run(void){
    serial_write("[shell] ready\n");
    fs_init();
    fs_persist_mount();          /* MuFS on /dev/hda; no-op without a disk */

    /* Seed the demo files only on a fresh namespace so that edits made on
     * a persistent MuFS survive the next boot. */
    if (fs_count() == 0) {
        fs_create("readme.txt");
        fs_write(fs_open("readme.txt"), "Welcome to MuOS 7!\nType 'help' for commands.\nType 'win7' for desktop.", 69);
        fs_create("notes.txt");
        fs_write(fs_open("notes.txt"), "MuOS 7 - x86 Microkernel\nBuilt with GCC 15 + NASM", 49);
    }

    /* Seed the RAM FS with the embedded Ring3 ELF so `exec hello.elf`
     * proves the loader also works on a filesystem image. Always refresh
     * it so a persisted stale binary can never shadow the current one. */
    uint32_t elf_size = 0;
    const void* elf = user_get_image(&elf_size);
    int elf_slot = fs_open("hello.elf");
    if (elf_slot < 0) elf_slot = fs_create("hello.elf");
    if (elf_slot >= 0 && elf_size > 0) {
        fs_write(elf_slot, (const char*)elf, elf_size);
    }

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
