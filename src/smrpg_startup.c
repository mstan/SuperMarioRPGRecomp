/* Launch logging reused from Super Metroid. Windows: no normal-launch console.
 * Both platforms write the session log to logs/ beside the executable unless
 * logging.ini asks for the console; explicit redirection is always kept. */
#include "smrpg_startup.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>

/* Shell pipes/files remain usable for scripts and benchmarks. Console handles
 * inherited by a GUI process are not necessarily attached or usable. */
static int redirected(FILE *stream, DWORD id) {
  DWORD type = GetFileType(GetStdHandle(id));
  return _fileno(stream) >= 0 && (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE);
}
static int open_log(const wchar_t *root, wchar_t *path, size_t count) {
  wchar_t folder[32768];
  if (swprintf(folder,32768,L"%ls\\logs",root)<0) return -1;
  if (!CreateDirectoryW(folder,NULL) && GetLastError()!=ERROR_ALREADY_EXISTS) return -1;
  SYSTEMTIME now; GetLocalTime(&now);
  if (swprintf(path,count,L"%ls\\smrpg-%04u%02u%02u-%02u%02u%02u-%lu.log",folder,
      now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,
      (unsigned long)GetCurrentProcessId())<0) return -1;
  return _wopen(path,_O_WRONLY|_O_CREAT|_O_EXCL|_O_BINARY,_S_IREAD|_S_IWRITE);
}
void SmrpgStartupLogging(void) {
  wchar_t root[32768],config[32768],path[32768];
  DWORD n=GetModuleFileNameW(NULL,root,32768);
  if (!n || n>=32768) return;
  wchar_t *slash=wcsrchr(root,L'\\'); if(!slash) return; *slash=0;
  if(swprintf(config,32768,L"%ls\\logging.ini",root)<0) return;
  int console=GetPrivateProfileIntW(L"Logging",L"Console",0,config)!=0;
  int out_redirected=redirected(stdout,STD_OUTPUT_HANDLE);
  int err_redirected=redirected(stderr,STD_ERROR_HANDLE);
  if(console && (GetConsoleWindow() || AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole())) {
    if(!out_redirected) freopen("CONOUT$","w",stdout);
    if(!err_redirected) freopen("CONOUT$","w",stderr);
    freopen("CONIN$","r",stdin);
  } else if(!out_redirected || !err_redirected) {
    int fd=open_log(root,path,32768);
    if(fd<0) {
      /* Read-only installations still retain diagnostics. */
      DWORD size=GetTempPathW(32768,root);
      if(size && size<32740) {
        wcscat(root,L"SuperMarioRPGSNESRecomp"); CreateDirectoryW(root,NULL);
        fd=open_log(root,path,32768);
      }
    }
    if(fd>=0) {
      /* GUI CRT streams may initially have descriptor -2. Opening NUL first
       * gives dup2 a real target descriptor without touching inherited pipes. */
      if(!out_redirected) { freopen("NUL","w",stdout); _dup2(fd,_fileno(stdout)); }
      if(!err_redirected) { freopen("NUL","w",stderr); _dup2(fd,_fileno(stderr)); }
      _close(fd);
    }
  }
  setvbuf(stdout,NULL,_IONBF,0); setvbuf(stderr,NULL,_IONBF,0);
  fprintf(stderr,"[smrpg] session pid=%lu; console=%s\n",
      (unsigned long)GetCurrentProcessId(),console?"enabled":"off");
}

/* The GUI subsystem avoids even a brief console flash before main. Both the
 * MinGW and MSVC CRTs supply the already-parsed narrow argument vector. */
extern int main(int argc,char **argv);
int WINAPI WinMain(HINSTANCE instance,HINSTANCE previous,LPSTR command,int show) {
  (void)instance;(void)previous;(void)command;(void)show;
  return main(__argc,__argv);
}
#else
#include "host_paths.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* A shell or script that sent output to a file or pipe keeps it there. */
static int redirected(int fd) {
  struct stat st;
  return fstat(fd,&st)==0 && (S_ISREG(st.st_mode) || S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode));
}
/* logging.ini [Logging] Console beside the executable; SMRPG_LOG_CONSOLE wins. */
static int console_requested(void) {
  const char *env=getenv("SMRPG_LOG_CONSOLE");
  if (env && *env) return *env!='0';
  char ini[1024],line[256];
  if (!snesrecomp_exe_dir_path("logging.ini",ini,sizeof(ini))) return 0;
  FILE *f=fopen(ini,"r");
  int console=0;
  while (f && fgets(line,sizeof(line),f)) {
    char *p=line;
    while (isspace((unsigned char)*p)) ++p;
    if (strncmp(p,"Console",7)) continue;
    p+=7;
    while (isspace((unsigned char)*p)) ++p;
    if (*p++!='=') continue;
    while (isspace((unsigned char)*p)) ++p;
    console=*p && *p!='0';
  }
  if (f) fclose(f);
  return console;
}
static int make_dirs(char *path) {
  for (char *p=path+1; *p; ++p) {
    if (*p!='/') continue;
    *p=0;
    int bad=mkdir(path,0755)!=0 && errno!=EEXIST;
    *p='/';
    if (bad) return -1;
  }
  return mkdir(path,0755)!=0 && errno!=EEXIST ? -1 : 0;
}
static int open_log(const char *folder,char *path,size_t count) {
  char dir[1024];
  if (snprintf(dir,sizeof(dir),"%s",folder)>=(int)sizeof(dir) || make_dirs(dir)) return -1;
  time_t now=time(NULL);
  struct tm *t=localtime(&now);
  char stamp[32]="unknown-time";
  if (t) strftime(stamp,sizeof(stamp),"%Y%m%d-%H%M%S",t);
  if (snprintf(path,count,"%s/smrpg-%s-%ld.log",folder,stamp,(long)getpid())>=(int)count) return -1;
  return open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0644);
}
void SmrpgStartupLogging(void) {
  int console=console_requested();
  int out_redirected=redirected(STDOUT_FILENO), err_redirected=redirected(STDERR_FILENO);
  char path[1200]="";
  if (!console && (!out_redirected || !err_redirected)) {
    char folder[1100];
    int fd=-1;
    /* Beside the executable (the .AppImage itself for AppImage builds), then
     * the XDG state directory, then /tmp, so a read-only install still logs. */
    if (snesrecomp_exe_dir_path("logs",folder,sizeof(folder))) fd=open_log(folder,path,sizeof(path));
    if (fd<0) {
      const char *state=getenv("XDG_STATE_HOME"),*home=getenv("HOME");
      if (state && *state) snprintf(folder,sizeof(folder),"%s/SuperMarioRPGSNESRecomp/logs",state);
      else if (home && *home) snprintf(folder,sizeof(folder),"%s/.local/state/SuperMarioRPGSNESRecomp/logs",home);
      else folder[0]=0;
      if (folder[0]) fd=open_log(folder,path,sizeof(path));
    }
    if (fd<0) fd=open_log("/tmp/SuperMarioRPGSNESRecomp",path,sizeof(path));
    if (fd>=0) {
      /* Tell a watching terminal where the output went, once, before it moves. */
      if (!err_redirected && isatty(STDERR_FILENO))
        fprintf(stderr,"[smrpg] logging to %s (SMRPG_LOG_CONSOLE=1 keeps output here)\n",path);
      fflush(stdout);fflush(stderr);
      if (!out_redirected) dup2(fd,STDOUT_FILENO);
      if (!err_redirected) dup2(fd,STDERR_FILENO);
      close(fd);
    }
  }
  setvbuf(stdout,NULL,_IONBF,0); setvbuf(stderr,NULL,_IONBF,0);
  fprintf(stderr,"[smrpg] session pid=%ld; console=%s\n",(long)getpid(),console?"enabled":"off");
}
#endif

/* Install before mod discovery, argument parsing, ROM loading or SDL setup. */
#include "host_report.h"
#include "desktop/post_mortem.h"
#include <signal.h>
#include <string.h>
static void exit_report(void) { recomp_post_mortem_dump("atexit",NULL); }
#ifdef _WIN32
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *info) {
  host_report_fatal("Unhandled host exception");
  recomp_post_mortem_dump("seh",info);
  return EXCEPTION_EXECUTE_HANDLER;
}
#endif
static void crash_signal(int number) {
  signal(number,SIG_DFL);
  host_report_fatal("Unhandled host signal");
  recomp_post_mortem_dump("signal",NULL);
  _Exit(128+number);
}
void SmrpgInstallCrashCapture(void) {
#ifdef _WIN32
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
  SetUnhandledExceptionFilter(crash_filter);
#endif
  signal(SIGABRT,crash_signal);signal(SIGSEGV,crash_signal);
  signal(SIGILL,crash_signal);signal(SIGFPE,crash_signal);
  atexit(exit_report);
}
void SmrpgCrashTestPhase(const char *phase) {
  const char *wanted=getenv("SMRPG_CRASH_TEST_PHASE");
  if(!wanted||strcmp(wanted,phase))return;
  fprintf(stderr,"[smrpg] intentional diagnostic crash drill: %s\n",phase);
  fflush(stderr);
#ifdef _WIN32
  RaiseException(0xe0425352u,EXCEPTION_NONCONTINUABLE,0,NULL);
#else
  raise(SIGABRT);
#endif
}
