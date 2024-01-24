#include "aiwn.h"
#include <signal.h>
// Look at your vendor's ucontext.h
#define __USE_GNU
#define DBG_MSG_RESUME   "%p:%d,RESUME,%p\n"   //(task,pid,single_step)
#define DBG_MSG_START    "%p:%d,START,%p\n"    //(task,pid,dump_regs_to)
#define DBG_MSG_SET_GREG "%p:%d,SGREG,%p,%p\n" //(task,pid,which_reg,value)
#define DBG_MSG_WATCH_TID                                                      \
  "0:0,WATCHTID,%d\n" //(tid,aiwnios is a Godsend,it will send you a message for
                      // the important TIDs(cores))
#define DBG_MSG_OK "OK"
#if defined(__linux__) || defined(__FreeBSD__)
  #include <poll.h>
  #include <sys/ptrace.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <ucontext.h>
  #include <unistd.h>
  #if defined(__FreeBSD__)
    #include <machine/reg.h>
  #endif
  #if defined(__linux__)
    #include <linux/elf.h> //Why do I need this for NT_PRSTATUS
    #include <sys/uio.h>
    #include <sys/user.h>
  #endif
#else
  #include <windows.h>
  #include <errhandlingapi.h>
  #include <handleapi.h>
  #include <processthreadsapi.h>
#endif
#if defined(__FreeBSD__)
typedef struct CFuckup {
  struct CQue  base;
  int64_t      signal;
  pid_t        pid;
  void        *task;
  struct reg   regs;
  struct fpreg fp;
} CFuckup;
  #define PTRACE_TRACEME   PT_TRACE_ME
  #define PTRACE_ATTACH    PT_ATTACH
  #define PTRACE_SETREGS   PT_SETREGS
  #define PTRACE_GETREGS   PT_GETREGS
  #define PTRACE_SETFPREGS PT_SETFPREGS
  #define PTRACE_GETFPREGS PT_GETFPREGS
  #define gettid           getpid
#endif
#if defined(__linux__)
  #include <asm/ptrace.h>
  #include <sys/user.h>
typedef struct CFuckup {
  struct CQue base;
  int64_t     signal;
  pid_t       pid;
  void       *task;
  #if defined(_M_ARM64) || defined(__aarch64__)
  struct user_pt_regs       regs;
  struct user_fpsimd_struct fp;
  #elif defined(__x86_64__)
  struct user_fpregs_struct fp;
  struct user_regs_struct   regs;
  #endif
} CFuckup;
#elif defined(WIN32) || defined(_WIN32)
  #include <winnt.h>
typedef struct CFuckup {
  struct CQue base;
  int64_t     signal;
  int64_t     pid; // Yeah GetThreadId
  void       *task;
  CONTEXT    *_regs; // regs may be alligned
} CFuckup;
  #ifndef SIGCONT
    #define SIGCONT 0x101
  #endif
#endif
CFuckup *GetFuckupByTask(CQue *head, void *t) {
  CFuckup *fu;
  for (fu = head->next; fu != head; fu = fu->base.next) {
    if (fu->task == t)
      return fu;
  }
  return NULL;
}
CFuckup *GetFuckupByPid(CQue *head, int64_t pid) {
  CFuckup *fu;
  for (fu = head->next; fu != head; fu = fu->base.next) {
    if (fu->pid == pid)
      return fu;
  }
  return NULL;
}
// Im making fuckups global on windows because debugging is not event driven on
// windows
static CQue fuckups;
#if defined(WIN32) || defined(_WIN32)
static int64_t gettid() {
  return GetCurrentThreadId();
}
// Windows aint unix so I will push P with  message Que
typedef struct _CDbgMsgQue {
  CQue base;
  char msg[0x100];
} _CDbgMsgQue;
static CQue   debugger_msgs;
static HANDLE debugger_mtx;
  #define HARD_CORE_CNT 64
static HANDLE  active_threads[HARD_CORE_CNT];
static CONTEXT thread_use_ctx[HARD_CORE_CNT];
static int64_t fault_codes[HARD_CORE_CNT];
static int64_t something_happened = 0;
static void    WriteMsg(char *buf) {
     WaitForSingleObject(debugger_mtx, INFINITE);
     _CDbgMsgQue *msg = malloc(sizeof(_CDbgMsgQue));
     QueIns(msg, debugger_msgs.last);
     strcpy(msg->msg, buf);
     ReleaseMutex(debugger_mtx);
}
static int64_t MsgPoll() {
  WaitForSingleObject(debugger_mtx, 5);
  int64_t r = QueCnt(&debugger_msgs) != 0;
  ReleaseMutex(debugger_mtx);
  return r;
}
static void ReadMsg(char *buf) {
  while (!MsgPoll())
    ;
  WaitForSingleObject(debugger_mtx, INFINITE);
  _CDbgMsgQue *msg = debugger_msgs.last;
  strcpy(buf, msg->msg);
  QueRem(msg);
  free(msg);
  ReleaseMutex(debugger_mtx);
}
static void GrabDebugger(int64_t code) {
  HANDLE  h = GetCurrentThread();
  int64_t tr;
  for (tr = 0; tr != HARD_CORE_CNT; tr++) {
    if (!active_threads[tr])
      break;
    if (GetThreadId(h) == GetThreadId(active_threads[tr]))
      goto found;
  }
  abort();
found:
  fault_codes[tr] = code;

  while (!Misc_LBts(&something_happened, tr)) {
    YieldProcessor(); // Wait for our "signal" to go through
  }
  SuspendThread(h);
}
static void AllowNext(int64_t pid) {
  int64_t tr;
  for (tr = 0; tr != HARD_CORE_CNT; tr++) {
    if (!active_threads[tr])
      break;
    if (GetThreadId(active_threads[tr]) == pid)
      goto found;
  }
  abort();
found:;
  ResumeThread(active_threads[tr]);
}
#else
static int  debugger_pipe[2];
static void ReadMsg(char *buf) {
  read(debugger_pipe[0], buf, 0x100);
}
static void WriteMsg(char *buf) {
  write(debugger_pipe[1], buf, 0x100);
}
static int64_t MsgPoll() {
  struct pollfd poll_for;
  memset(&poll_for, 0, sizeof(struct pollfd));
  poll_for.fd     = debugger_pipe[0];
  poll_for.events = POLL_IN;
  if (poll(&poll_for, 1, 2))
    return 1;
  return 0;
}
static void GrabDebugger(int64_t code) {
  raise(code);
}
#endif

void DebuggerClientWatchThisTID() {
#if defined(_WIN32) || defined(WIN32)
  int64_t tr;
  WaitForSingleObject(debugger_mtx, INFINITE);
  for (tr = 0; active_threads[tr]; tr++)
    ;

  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                  &active_threads[tr], 0, FALSE, DUPLICATE_SAME_ACCESS);
  ReleaseMutex(debugger_mtx);
#else
  int64_t tid = gettid();
  char    buf[256];
  sprintf(buf, DBG_MSG_WATCH_TID, tid);
  WriteMsg(buf);
#endif
}
#if defined(_WIN32) || defined(WIN32)
static int64_t DebuggerWait(CQue *head, pid_t *got) {
  int64_t  s, tid;
  CFuckup *fu;
  for (s = 0; s != HARD_CORE_CNT; s++) {
    if (Misc_LBtr(&something_happened, s)) {
      tid = GetThreadId(active_threads[s]);
      if (fu = GetFuckupByPid(head, tid))
        ;
      else {
        abort();
      }
      fu->signal     = fault_codes[s];
      fault_codes[s] = 0;
      if (got)
        *got = tid;
      return 1;
    }
  }
  return 0;
}
#else
static int64_t DebuggerWait(CQue *head, pid_t *got) {
  int      code, sig;
  pid_t    pid = waitpid(-1, &code, WNOHANG | WUNTRACED | WCONTINUED);
  CFuckup *fu;
  if (pid <= 0)
    return 0;
  if (WIFEXITED(code)) {
    close(debugger_pipe[0]);
    close(debugger_pipe[1]);
    ptrace(PT_DETACH, pid, 0, 0);
    exit(WEXITSTATUS(code));
    return 0;
  } else if (WIFSIGNALED(code) || WIFSTOPPED(code) || WIFCONTINUED(code)) {
    if (WIFSIGNALED(code))
      sig = WTERMSIG(code);
    else if (WIFSTOPPED(code))
      sig = WSTOPSIG(code);
    else if (WIFCONTINUED(code))
      sig = SIGCONT;
    else
      abort();
    if (fu = GetFuckupByPid(head, pid))
      fu->signal = sig;
    else {
      fu = calloc(sizeof(CFuckup), 1);
      QueIns(fu, head);
      fu->pid = pid;
    }
    switch (sig) {
    case SIGCONT:
    case SIGSTOP:
      break;
    default:
  #if defined(__x86_64__)
      // IF you are blessed you are running on a platform that has these
      // Here's the deal Linux takes it in data/freebsd takes it in
      // addr(only 1 is used my homie)
      ptrace(PTRACE_GETREGS, pid, &fu->regs, &fu->regs);
      ptrace(PTRACE_GETFPREGS, pid, &fu->fp, &fu->fp);
  #elif defined(PTRACE_GETREGSET)
      struct iovec poop;
      poop.iov_len  = sizeof(fu->regs);
      poop.iov_base = &fu->regs;
      ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &poop);
      poop.iov_len  = sizeof(fu->fp);
      poop.iov_base = &fu->fp;
      ptrace(PTRACE_GETREGSET, pid, NT_PRFPREG, &poop);
  #endif
    }
    if (got)
      *got = pid;
    return sig;
  }
}
#endif
static void PTWriteAPtr(int64_t tid, void *to, uint64_t v) {
  int64_t s;
#if defined(_WIN32) || defined(WIN32)
  *(void **)to = (void *)v;
#endif
#if defined(__linux__)
  #if defined(_M_ARM64) || defined(__aarch64__) || defined(__x86_64__)
  for (s = 0; s != 8 / 2; s++) {
    if (!s)
      ptrace(PTRACE_POKETEXT, tid, to + s, v & 0xffff);
    else
      ptrace(PTRACE_POKETEXT, tid, to + s * 2, (v >> (s * 16ul)) & 0xfffful);
  }
  #endif
#elif defined(__FreeBSD__)
  for (s = 0; s != 8 / 4; s++)
    ptrace(PT_WRITE_D, tid, to + s * 4, (v >> (s * 32ul)) & 0xffffFFFFul);
#endif
}
void DebuggerBegin() {
  pid_t tid = 0;
#if defined(_WIN32) || defined(WIN32)
  static int64_t init = 0;
  tid                 = init; // SIMULATE fork sort of
  if (!init) {
    memset(active_threads, 0, 8 * HARD_CORE_CNT);
    memset(fault_codes, 0, 8 * HARD_CORE_CNT);
    debugger_mtx = CreateMutexA(NULL, 0, NULL);
    QueInit(&debugger_msgs);
    init = 1;
  }
#else
  pipe(debugger_pipe);
  tid = fork();
#endif
  int   code, sig;
  void *task;
  char  buf[4048], *ptr;
  pid_t child = tid;
  if (!tid) {
#if !(defined(_WIN32) || defined(WIN32))
    ptrace(PTRACE_TRACEME, tid, NULL, NULL);
#else
    CreateThread(NULL, 0, &DebuggerBegin, NULL, 0, NULL);
#endif
    strcpy(buf, DBG_MSG_OK);
    WriteMsg(buf);
    return;
  } else {
#if !(defined(_WIN32) || defined(WIN32))
    ptrace(PT_ATTACH, tid, NULL, NULL);
    wait(NULL);
    ptrace(PT_CONTINUE, tid, 1, NULL);
#endif
    while (1) {
      ReadMsg(buf);
      if (strcmp(buf, DBG_MSG_OK))
        exit(1);
      else
        break;
    }

#if defined(__linux__)
    struct iovec poop;
#endif
    CFuckup *fu, *last;
    char    *ptr, *name;
    int64_t  which, value, sig, *write_regs_to;
    QueInit(&fuckups);
    while (1) {
      if (MsgPoll()) {
        ReadMsg(buf);
        ptr  = buf;
        task = NULL;
        while (1) {
          if (*ptr == ':') {
            tid = 0;
            sscanf(buf, "%p:%d", &task, &tid);
          } else if (*ptr == ',') {
            name = ptr + 1;
            if (!strncmp(name, "SGREG", strlen("SGREG"))) {
              void *task;
              int   gott;
              sscanf(buf, DBG_MSG_SET_GREG, &task, &gott, &which, &value);

              while (1) {
                DebuggerWait(&fuckups, NULL);
                if (fu = GetFuckupByPid(&fuckups, tid)) {
                  if (fu->signal == SIGCONT) {
                    fu->signal = 0;
                    break;
                  }
                }
              }
              // SEE swapctxX86.s
#if defined(_WIN32) || defined(WIN32)
              switch (which) {
              case 0:
                fu->_regs->Rip = value;
                break;
              case 1:
                fu->_regs->Rsp = value;
                break;
              case 2:
                fu->_regs->Rbp = value;
                break;
              }
              AllowNext(tid);
#endif
#if defined(__FreeBSD__) && defined(__x86_64__)
              switch (which) {
              case 0:
                fu->regs.r_rip = value;
                break;
              case 1:
                fu->regs.r_rsp = value;
                break;
              case 2:
                fu->regs.r_rbp = value;
                break;
                // DONT RELY ON CHANGING GPs
              }
              ptrace(PT_CONTINUE, tid, 1, 0);
#endif
#if defined(__linux__) && defined(__x86_64__)
              switch (which) {
              case 0:
                fu->regs.rip = value;
                break;
              case 1:
                fu->regs.rsp = value;
                break;
              case 2:
                fu->regs.rbp = value;
                break;
                // DONT RELY ON CHANGING GPs
              }
              ptrace(PT_CONTINUE, tid, 0, 0);
#endif
#if defined(__linux__) && (defined(_M_ARM64) || defined(__aarch64__))
              switch (which) {
              case 0:
                fu->regs.pc = value;
                break;
              case 21:
                fu->regs.sp = value;
                break;
              case 11:
                fu->regs.regs[29] = value;
                break;
                // DONT RELY ON CHANGING GPs
              }
              ptrace(PT_CONTINUE, tid, 0, 0);
#endif
              break;
            } else if (!strncmp(name, "WATCHTID", strlen("WATCHTID"))) {
              int64_t tr;
              ptr = name + strlen("WATCHTID");
              if (*ptr != ',')
                abort();
              ptr++;
// On FreeBSD there is no "TID"s
#if defined(__linux__)
              ptrace(PTRACE_ATTACH, strtoul(ptr, NULL, 10), NULL, NULL);
#endif
              break;
            } else if (!strncmp(name, "START", strlen("START"))) {
              void *task;
              int   gott;
              sscanf(buf, DBG_MSG_START, &task, &gott, &write_regs_to);
              while (1) {
                DebuggerWait(&fuckups, NULL);
                if (fu = GetFuckupByPid(&fuckups, tid)) {
                  if (fu->signal == SIGCONT) {
                    fu->signal = 0;
                    fu->task   = task;
                    break;
                  }
                }
              }
              if (!fu)
                puts("undef Fu");
              else {
// See your swapctx for your arch(swapctxAARCH64/swapctxX86)
#if defined(__x86_64__)
  #if defined(__linux__)
                PTWriteAPtr(tid, &write_regs_to[0], fu->regs.rip);
                PTWriteAPtr(tid, &write_regs_to[1], fu->regs.rsp);
                PTWriteAPtr(tid, &write_regs_to[2], fu->regs.rbp);
  #elif defined(__FreeBSD__)
                PTWriteAPtr(tid, &write_regs_to[0], fu->regs.r_rip);
                PTWriteAPtr(tid, &write_regs_to[1], fu->regs.r_rsp);
                PTWriteAPtr(tid, &write_regs_to[2], fu->regs.r_rbp);
  #elif defined(_WIN32) || defined(WIN32)
                PTWriteAPtr(tid, &write_regs_to[0], fu->_regs->Rip);
                PTWriteAPtr(tid, &write_regs_to[1], fu->_regs->Rsp);
                PTWriteAPtr(tid, &write_regs_to[2], fu->_regs->Rbp);
  #endif
#endif

#if defined(_M_ARM64) || defined(__aarch64__)
  #if defined(__linux__)
                PTWriteAPtr(tid, &write_regs_to[0], fu->regs.pc);
                PTWriteAPtr(tid, &write_regs_to[21], fu->regs.sp);
                PTWriteAPtr(tid, &write_regs_to[11], fu->regs.regs[29]);
  #elif defined(__FreeBSD__)
  #endif
#endif
              }
#if defined(_WIN32) || defined(WIN32)
              AllowNext(tid);
#else
              ptrace(PT_CONTINUE, tid, 1, 0);
#endif
              break;
            } else if (!strncmp(name, "RESUME", strlen("RESUME"))) {
              void   *task;
              int     gott;
              int64_t ss;
              sscanf(buf, DBG_MSG_RESUME, &task, &gott, &ss);
              while (1) {
                DebuggerWait(&fuckups, NULL);
                if (fu = GetFuckupByPid(&fuckups, tid)) {
                  if (fu->signal == SIGCONT) {
                    fu->signal = 0;
                    break;
                  }
                }
              }
              fu = GetFuckupByTask(&fuckups, task);
              if (!fu) {
#if defined(_WIN32) || defined(WIN32)
                AllowNext(tid);
#else
                ptrace(PT_CONTINUE, tid, 1, 0);
#endif
                break;
              }
#if !(defined(_WIN32) || defined(WIN32))
  #if defined(__x86_64__)
              // IF you are blessed you are running on a platform that has
              // these Here's the deal Linux takes it in data/freebsd takes it
              // in addr(only 1 is used my homie)
              ptrace(PTRACE_SETREGS, tid, &fu->regs, &fu->regs);
              ptrace(PTRACE_SETFPREGS, tid, &fu->fp, &fu->fp);
  #else
              poop.iov_len  = sizeof(fu->regs);
              poop.iov_base = &fu->regs;
              ptrace(PTRACE_SETREGSET, tid, NT_PRSTATUS, &poop);
              poop.iov_len  = sizeof(fu->fp);
              poop.iov_base = &fu->fp;
              ptrace(PTRACE_SETREGSET, tid, NT_PRFPREG, &poop);
  #endif
#else
              // See thread_use_ctx
              int64_t tr;
              for (tr = 0; tr != HARD_CORE_CNT; tr++) {
                if (!active_threads[tr])
                  abort();
                if (GetThreadId(active_threads[tr]) == tid)
                  break;
              }
#endif
              if (ss) {
#if defined(__FreeBSD__)
                ptrace(PT_STEP, tid, 1, 0);
#elif defined(_WIN32) || defined(WIN32)
                fu->_regs->EFlags |= 1 << 8; // Trap flag
                AllowNext(tid);
#else
                ptrace(PTRACE_SINGLESTEP, tid, 0, 0);
#endif
                QueRem(fu);
                free(fu);
              } else {
#if defined(_WI32) || defined(WIN32)
                AllowNext(tid);
#else
  #if defined(__x86_64__)
                ptrace(PT_CONTINUE, tid, 1, 0);
  #else
                ptrace(PT_CONTINUE, tid, 0, 0);
  #endif
#endif
                QueRem(fu);
                free(fu);
              }
            } else
              abort();
            break;
          }
          ptr++;
        }
      } else if (sig = DebuggerWait(&fuckups, &tid)) {
#if defined(_WIN32) || defined(WIN32)
#else
        if (sig == SIGCONT)
          ;
        else if (sig == SIGSTOP) // This is used for ptrace events
          ptrace(PT_CONTINUE, tid, 1, 0);
        else
          ptrace(PT_CONTINUE, tid, 1, sig);
#endif
      }
    }
  }
}
static void UnblockSignals() {
#if defined(__linux__) || defined(__FreeBSD__)
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGILL);
  sigaddset(&set, SIGSEGV);
  sigaddset(&set, SIGBUS);
  sigaddset(&set, SIGTRAP);
  sigaddset(&set, SIGFPE);
  sigprocmask(SIG_UNBLOCK, &set, NULL);
#endif
}
#if defined(__linux__) || defined(__FreeBSD__)
static void SigHandler(int64_t sig, siginfo_t *info, ucontext_t *_ctx) {
  #if defined(__x86_64__)
    #if defined(__linux__)
  // See /usr/include/x86_64-linux-gnu/sys/ucontext.h
  enum {
    REG_R8 = 0,
    REG_R9,
    REG_R10,
    REG_R11,
    REG_R12,
    REG_R13,
    REG_R14,
    REG_R15,
    REG_RDI,
    REG_RSI,
    REG_RBP,
    REG_RBX,
    REG_RDX,
    REG_RAX,
    REG_RCX,
    REG_RSP,
    REG_RIP,
  };
  UnblockSignals();
  mcontext_t *ctx = &_ctx->uc_mcontext;
  int64_t     actx[32];
  actx[0] = ctx->gregs[REG_RIP];
  actx[1] = ctx->gregs[REG_RSP];
  actx[2] = ctx->gregs[REG_RBP];
  actx[3] = ctx->gregs[REG_R11];
  actx[4] = ctx->gregs[REG_R12];
  actx[5] = ctx->gregs[REG_R13];
  actx[6] = ctx->gregs[REG_R14];
  actx[7] = ctx->gregs[REG_R15];
  CHashExport *exp;
  if (exp = HashFind("AiwniosDbgCB", Fs->hash_table, HTT_EXPORT_SYS_SYM, 1)) {
    FFI_CALL_TOS_2(exp->val, sig, actx);
  } else if (exp = HashFind("Exit", Fs->hash_table, HTT_EXPORT_SYS_SYM, 1)) {
    ctx->gregs[0] = exp->val;
  } else
    abort();
  setcontext(_ctx);
    #elif defined(__FreeBSD__)
  UnblockSignals();
  mcontext_t *ctx = &_ctx->uc_mcontext;
  int64_t     actx[32];
  actx[0] = ctx->mc_rip;
  actx[1] = ctx->mc_rsp;
  actx[2] = ctx->mc_rbp;
  actx[3] = ctx->mc_r11;
  actx[4] = ctx->mc_r12;
  actx[5] = ctx->mc_r13;
  actx[6] = ctx->mc_r14;
  actx[7] = ctx->mc_r15;
  CHashExport *exp;
  if (exp = HashFind("AiwniosDbgCB", Fs->hash_table, HTT_EXPORT_SYS_SYM, 1)) {
    FFI_CALL_TOS_2(exp->val, sig, actx);
  } else if (exp = HashFind("Exit", Fs->hash_table, HTT_EXPORT_SYS_SYM, 1)) {
    ctx->mc_rip = exp->val;
  } else
    abort();
  setcontext(_ctx);
    #endif
  #elif defined(__aarch64__) || defined(_M_ARM64)
  mcontext_t  *ctx = &_ctx->uc_mcontext;
  CHashExport *exp;
  int64_t      is_single_step;
  // See swapctxAARCH64.s
  //  I have a secret,im only filling in saved registers as they are used
  //  for vairables in Aiwnios. I dont have plans on adding tmp registers
  //  in here anytime soon
  int64_t (*fp)(int64_t sig, int64_t * ctx), (*fp2)();
  int64_t actx[(30 - 18 + 1) + (15 - 8 + 1) + 1];
  int64_t i, i2, sz, fp_idx;
  UnblockSignals();
  for (i = 18; i <= 30; i++)
    if (i - 18 != 12) // the 12th one is the return register
    #if defined(__linux__)
      actx[i - 18] = ctx->regs[i];
    #elif defined(__FreeBSD__)
      actx[i - 18] = ctx->mc_gpregs.gp_x[i];
    #endif
    #if defined(__linux__)
  // We will look for FPSIMD_MAGIC(0x46508001)
  // From
  // https://github.com/torvalds/linux/blob/master/arch/arm64/include/uapi/asm/sigcontext.h
  for (i = 0; i < 4096;) {
    sz = ((uint32_t *)(&ctx->__reserved[i]))[1];
    if (((uint32_t *)(&ctx->__reserved[i]))[0] == 0x46508001) {
      i += 4 + 4 + 4 + 4;
      fp_idx = i;
      for (i2 = 8; i2 <= 15; i2++)
        actx[(30 - 18 + 1) + i2 - 8] =
            *(int64_t *)(&ctx->__reserved[fp_idx + 16 * i2]);
      break;
    } else if (!sz)
      break;
    else
      i += sz;
  }
    #elif defined(__FreeBSD__)
  for (i2 = 8; i2 <= 15; i2++)
    actx[(30 - 18 + 1) + i2 - 8] = *(int64_t *)(&ctx->mc_fpregs.fp_q[16 * i2]);
    #endif
    #if defined(__linux__)
  actx[21] = ctx->sp;
    #elif defined(__FreeBSD__)
  actx[21] = ctx->mc_gpregs.gp_sp;
    #endif
  // AiwniosDbgCB will return 1 for singlestep
  if (exp = HashFind("AiwniosDbgCB", Fs->hash_table, HTT_EXPORT_SYS_SYM, 1)) {
    fp = exp->val;
    if (FFI_CALL_TOS_2(fp, sig, actx)) { // Returns 1 for single step
    #if defined(__x86_64__)
      #if defined(__FreeBSD__)
      ctx->mc_rip = actx[0];
      ctx->mc_rsp = actx[1];
      ctx->mc_rbp = actx[2];
      ctx->mc_r11 = actx[3];
      ctx->mc_r12 = actx[4];
      ctx->mc_r13 = actx[5];
      ctx->mc_r14 = actx[6];
      ctx->mc_r15 = actx[7];
      ctx->mc_eflags |= 1 << 8;
      #elif defined(__linux__)
      ctx->gregs[REG_RIP] = actx[0];
      ctx->gregs[REG_RSP] = actx[1];
      ctx->gregs[REG_RBP] = actx[2];
      ctx->gregs[REG_R11] = actx[3];
      ctx->gregs[REG_R12] = actx[4];
      ctx->gregs[REG_R13] = actx[5];
      ctx->gregs[REG_R14] = actx[6];
      ctx->gregs[REG_R15] = actx[7];
      #endif
    #endif
    }
  } else if (exp = HashFind("Exit", Fs->hash_table, HTT_EXPORT_SYS_SYM, 1)) {
  call_exit:
    fp2 = exp->val;
    FFI_CALL_TOS_0(fp2);
  } else
    abort();
  #endif
}
#endif
#if defined(WIN32) || defined(_WIN32)
static int64_t VectorHandler(struct _EXCEPTION_POINTERS *einfo) {
  CONTEXT *ctx = einfo->ContextRecord;
  CFuckup *fu  = calloc(1, sizeof(CFuckup));
  int64_t  sig = 0;
  QueIns(fu, &fuckups);
  fu->pid   = GetCurrentThreadId();
  fu->_regs = ctx;
  int64_t actx[23];
  actx[0] = ctx->Rip;
  actx[1] = ctx->Rsp;
  actx[2] = ctx->Rbp;
  CHashExport *exp;
  if (exp = HashFind("AiwniosDbgCB", Fs->hash_table, HTT_EXPORT_SYS_SYM, 1)) {
    switch (einfo->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_SINGLE_STEP:
    case EXCEPTION_BREAKPOINT:
      sig = 5;
    }
    FFI_CALL_TOS_2(exp->val, sig, actx);
  } else if (exp = HashFind("Exit", Fs->hash_table, HTT_EXPORT_SYS_SYM, 1))
    FFI_CALL_TOS_0(exp->val);
  else
    abort();
fin:
  return EXCEPTION_CONTINUE_EXECUTION;
}
#endif
void InstallDbgSignalsForThread() {
#if defined(__linux__) || defined(__FreeBSD__)
  struct sigaction sa;
  memset(&sa, 0, sizeof(struct sigaction));
  sa.sa_handler   = SIG_IGN;
  sa.sa_flags     = SA_SIGINFO;
  sa.sa_sigaction = (void *)&SigHandler;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGTRAP, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGFPE, &sa, NULL);
#else
  AddVectoredExceptionHandler(1, VectorHandler);
#endif
}
// This happens when after we call DebuggerClientEnd
void DebuggerClientSetGreg(void *task, int64_t which, int64_t v) {
  char buf[256];
  sprintf(buf, DBG_MSG_SET_GREG, task, gettid(), which, v);
  WriteMsg(buf);
  GrabDebugger(SIGCONT);
}
void DebuggerClientStart(void *task, void **write_regs_to) {
  char buf[256], *ptr;
  sprintf(buf, DBG_MSG_START, task, gettid(), write_regs_to);
  WriteMsg(buf);
  GrabDebugger(SIGCONT);
}
void DebuggerClientEnd(void *task, int64_t wants_singlestep) {
  char buf[256];
  sprintf(buf, DBG_MSG_RESUME, task, gettid(), wants_singlestep);
  WriteMsg(buf);
  GrabDebugger(SIGCONT);
}
