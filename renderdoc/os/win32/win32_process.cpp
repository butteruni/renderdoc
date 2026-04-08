/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2026 Baldur Karlsson
 * Copyright (c) 2014 Crytek
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

// must be separate so that it's included first and not sorted by clang-format
#include <windows.h>

#include <Psapi.h>
#include <tchar.h>
#include <tlhelp32.h>
#include "common/formatting.h"
#include "core/core.h"
#include "os/os_specific.h"
#include "strings/string_utils.h"

#include <string>
#include <map>

static rdcarray<EnvironmentModification> &GetEnvModifications()
{
  static rdcarray<EnvironmentModification> envCallbacks;
  return envCallbacks;
}

struct InsensitiveComparison
{
  bool operator()(const rdcstr &a, const rdcstr &b) const { return strlower(a) < strlower(b); }
};

typedef std::map<rdcstr, rdcstr, InsensitiveComparison> EnvMap;

static EnvMap EnvStringToEnvMap(const wchar_t *envstring)
{
  EnvMap ret;

  const wchar_t *e = envstring;

  while(*e)
  {
    const wchar_t *equals = wcschr(e, L'=');

    rdcstr name = StringFormat::Wide2UTF8(rdcwstr(e, equals - e));
    rdcstr value = StringFormat::Wide2UTF8(equals + 1);

    ret[name] = value;

    // jump to \0 and past it
    e += wcslen(e) + 1;
  }

  return ret;
}

void Process::RegisterEnvironmentModification(const EnvironmentModification &modif)
{
  GetEnvModifications().push_back(modif);
}

static void ApplyEnvModifications(EnvMap &envValues,
                                  const rdcarray<EnvironmentModification> &modifications,
                                  bool setToSystem)
{
  for(size_t i = 0; i < modifications.size(); i++)
  {
    const EnvironmentModification &m = modifications[i];

    rdcstr value;

    auto it = envValues.find(m.name);
    if(it != envValues.end())
      value = it->second;

    switch(m.mod)
    {
      case EnvMod::Set: value = m.value.c_str(); break;
      case EnvMod::Append:
      {
        if(!value.empty())
        {
          if(m.sep == EnvSep::Platform || m.sep == EnvSep::SemiColon)
            value += ";";
          else if(m.sep == EnvSep::Colon)
            value += ":";
        }
        value += m.value.c_str();
        break;
      }
      case EnvMod::Prepend:
      {
        if(!value.empty())
        {
          rdcstr prep = m.value;
          if(m.sep == EnvSep::Platform || m.sep == EnvSep::SemiColon)
            prep += ";";
          else if(m.sep == EnvSep::Colon)
            prep += ":";
          value = prep + value;
        }
        else
        {
          value = m.value.c_str();
        }
        break;
      }
    }

    envValues[m.name] = value;

    if(setToSystem)
      SetEnvironmentVariableW(StringFormat::UTF82Wide(m.name).c_str(),
                              StringFormat::UTF82Wide(value).c_str());
  }
}

// on windows we apply environment changes here, after process initialisation
// but before any real work (in RenderDoc::Initialise) so that we support
// injecting the dll into processes we didn't launch (ie didn't control the
// starting environment for), or even the application loading the dll itself
// without any interaction with our replay app.
void Process::ApplyEnvironmentModification()
{
  // turn environment string to a UTF-8 map
  LPWCH envStrings = GetEnvironmentStringsW();
  EnvMap envValues = EnvStringToEnvMap(envStrings);
  FreeEnvironmentStringsW(envStrings);
  rdcarray<EnvironmentModification> &modifications = GetEnvModifications();

  ApplyEnvModifications(envValues, modifications, true);

  // these have been applied to the current process
  modifications.clear();
}

rdcstr Process::GetEnvVariable(const rdcstr &name)
{
  DWORD len = GetEnvironmentVariableA(name.c_str(), NULL, 0);
  if(len == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
    return rdcstr();

  rdcstr ret;
  ret.resize(len + 1);

  GetEnvironmentVariableA(name.c_str(), ret.data(), len);
  ret.trim();
  return ret;
}

uint64_t Process::GetMemoryUsage()
{
  HANDLE proc = GetCurrentProcess();

  if(proc == NULL)
  {
    RDCERR("Couldn't open process: %d", GetLastError());
    return 0;
  }

  PROCESS_MEMORY_COUNTERS memInfo = {};

  uint64_t ret = 0;

  if(GetProcessMemoryInfo(proc, &memInfo, sizeof(memInfo)))
  {
    ret = memInfo.WorkingSetSize;
  }
  else
  {
    RDCERR("Couldn't get process memory info: %d", GetLastError());
  }

  return ret;
}

// helpers for various shims and dlls etc, not part of the public API
extern "C" __declspec(dllexport) void __cdecl INTERNAL_GetTargetControlIdent(uint32_t *ident)
{
  if(ident)
    *ident = RenderDoc::Inst().GetTargetControlIdent();
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_SetCaptureOptions(CaptureOptions *opts)
{
  if(opts)
    RenderDoc::Inst().SetCaptureOptions(*opts);
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_SetCaptureFile(const char *capfile)
{
  if(capfile)
    RenderDoc::Inst().SetCaptureFileTemplate(capfile);
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_SetDebugLogFile(const char *logfile)
{
  RENDERDOC_SetDebugLogFile(logfile ? logfile : rdcstr());
}

static EnvironmentModification tempEnvMod;

extern "C" __declspec(dllexport) void __cdecl INTERNAL_EnvModName(const char *name)
{
  if(name)
    tempEnvMod.name = name;
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_EnvModValue(const char *value)
{
  if(value)
    tempEnvMod.value = value;
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_EnvSep(EnvSep *sep)
{
  if(sep)
    tempEnvMod.sep = *sep;
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_EnvMod(EnvMod *mod)
{
  if(mod)
  {
    tempEnvMod.mod = *mod;
    Process::RegisterEnvironmentModification(tempEnvMod);
  }
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_ApplyEnvMods(void *ignored)
{
  Process::ApplyEnvironmentModification();
}

// 通过 CreateRemoteThread 方式注入 DLL（传统方式）
static bool InjectDLL_CreateRemoteThread(HANDLE hProcess, const wchar_t *dllPath, size_t dllPathSize)
{
  static HMODULE kernel32 = GetModuleHandleA("kernel32.dll");

  if(kernel32 == NULL)
  {
    RDCERR("Couldn't get handle for kernel32.dll");
    return false;
  }

  void *remoteMem =
      VirtualAllocEx(hProcess, NULL, dllPathSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  if(!remoteMem)
  {
    RDCERR("Couldn't allocate remote memory for DLL: %u", GetLastError());
    return false;
  }

  BOOL success = WriteProcessMemory(hProcess, remoteMem, (void *)dllPath, dllPathSize, NULL);
  if(!success)
  {
    RDCERR("Couldn't write remote memory %p with dllPath: %u", remoteMem, GetLastError());
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    return false;
  }

  HANDLE hThread = CreateRemoteThread(
      hProcess, NULL, 1024 * 1024U,
      (LPTHREAD_START_ROUTINE)GetProcAddress(kernel32, "LoadLibraryW"), remoteMem, 0, NULL);
  if(!hThread)
  {
    RDCERR("Couldn't create remote thread for LoadLibraryW: %u", GetLastError());
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    return false;
  }

  WaitForSingleObject(hThread, INFINITE);
  CloseHandle(hThread);
  VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
  return true;
}

// 记录每个进程的主线程 ID，避免 LoadLibraryW 后新线程干扰查找
static std::map<DWORD, DWORD> s_MainThreadIds;

// 查找目标进程的主线程句柄（用于 SetThreadContext 注入）
// 返回的线程保证处于挂起状态（挂起计数 >= 1）
// 如果之前已经记录了该进程的主线程 ID，则直接使用，
// 避免 LoadLibraryW 创建新线程后找错线程。
static HANDLE FindMainThread(DWORD pid)
{
  // 如果已经记录了主线程 ID，直接打开它
  auto it = s_MainThreadIds.find(pid);
  if(it != s_MainThreadIds.end())
  {
    HANDLE hThread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                                FALSE, it->second);
    if(hThread)
    {
      SuspendThread(hThread);
      RDCLOG("Reusing known main thread %u for process %u", it->second, pid);
      return hThread;
    }
    // 如果打开失败（线程已退出），清除记录，走下面的查找逻辑
    s_MainThreadIds.erase(it);
  }

  HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if(hSnapshot == INVALID_HANDLE_VALUE)
    return NULL;

  THREADENTRY32 te32;
  te32.dwSize = sizeof(THREADENTRY32);

  HANDLE hThread = NULL;
  DWORD earliestThread = 0;

  if(Thread32First(hSnapshot, &te32))
  {
    do
    {
      if(te32.th32OwnerProcessID == pid)
      {
        // 取第一个找到的线程（通常是主线程）
        if(hThread == NULL)
        {
          hThread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                               FALSE, te32.th32ThreadID);
          earliestThread = te32.th32ThreadID;
        }
      }
    } while(Thread32Next(hSnapshot, &te32));
  }

  CloseHandle(hSnapshot);

  if(hThread)
  {
    // 显式挂起线程，确保 GetThreadContext/SetThreadContext 能正确工作。
    // 即使线程已经是挂起状态（如 CREATE_SUSPENDED 创建的进程），
    // SuspendThread 也只是增加挂起计数，不会出错。
    // 后续需要对应的 ResumeThread 来抵消这次挂起。
    SuspendThread(hThread);
    // 记录主线程 ID，后续 InjectFunctionCall 可以直接使用
    s_MainThreadIds[pid] = earliestThread;
    RDCLOG("Found and suspended main thread %u for process %u", earliestThread, pid);
  }

  return hThread;
}

// 通过 SetThreadContext 方式注入 DLL（劫持挂起线程的方式）
// 原理：修改挂起线程的指令指针，使其先执行我们的 shellcode（调用 LoadLibraryW），
// 然后跳回原始的指令地址继续执行。
// 使用标志位（flag）同步：shellcode 在 LoadLibraryW 完成后写入标志位，注入方轮询检查。
//
// 改进：shellcode 会保存/恢复所有被修改的寄存器，确保跳回原始 IP 时寄存器状态完整。
// shellcode 在设置完成标志后会调用 SuspendThread(-2) 挂起自身，
// 避免外部竞态挂起导致的死锁问题。
static bool InjectDLL_SetThreadContext(HANDLE hProcess, DWORD pid, const wchar_t *dllPath,
                                       size_t dllPathSize)
{
  // 查找目标进程的主线程（FindMainThread 会确保线程处于挂起状态）
  HANDLE hThread = FindMainThread(pid);
  if(!hThread)
  {
    RDCWARN("SetThreadContext injection: couldn't find main thread for process %u", pid);
    return false;
  }

  static HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
  if(kernel32 == NULL)
  {
    RDCERR("Couldn't get handle for kernel32.dll");
    CloseHandle(hThread);
    return false;
  }

  void *pLoadLibraryW = (void *)GetProcAddress(kernel32, "LoadLibraryW");
  if(!pLoadLibraryW)
  {
    RDCERR("Couldn't get address of LoadLibraryW");
    CloseHandle(hThread);
    return false;
  }

  // 获取 GetCurrentThread 和 SuspendThread 的地址，
  // shellcode 将调用 SuspendThread(GetCurrentThread()) 来安全地挂起自身。
  void *pGetCurrentThread = (void *)GetProcAddress(kernel32, "GetCurrentThread");
  void *pSuspendThread = (void *)GetProcAddress(kernel32, "SuspendThread");
  if(!pGetCurrentThread || !pSuspendThread)
  {
    RDCERR("Couldn't get address of GetCurrentThread/SuspendThread");
    CloseHandle(hThread);
    return false;
  }

  // 获取线程上下文
  CONTEXT ctx = {};
  ctx.ContextFlags = CONTEXT_FULL;
  if(!GetThreadContext(hThread, &ctx))
  {
    RDCERR("SetThreadContext injection: GetThreadContext failed: %u", GetLastError());
    CloseHandle(hThread);
    return false;
  }

#if ENABLED(RDOC_X64)
  DWORD64 origIP = ctx.Rip;
  DWORD64 origRCX = ctx.Rcx;
  DWORD64 origRDX = ctx.Rdx;
  DWORD64 origR8 = ctx.R8;
  DWORD64 origR9 = ctx.R9;
  DWORD64 origR10 = ctx.R10;
  DWORD64 origR11 = ctx.R11;
  DWORD64 origRAX = ctx.Rax;
#else
  DWORD origIP = ctx.Eip;
#endif

  // 内存布局：
  //   [shellcode 代码] [DLL 路径字符串] [完成标志 DWORD，初始为 0] [保存的上下文数据]
  //
  // shellcode 的功能：
  // 1. 保存所有易失性寄存器到栈上
  // 2. 调用 LoadLibraryW(dllPath)
  // 3. 将完成标志设为 1（通知注入方 LoadLibraryW 已完成）
  // 4. 调用 SuspendThread(GetCurrentThread()) 挂起自身
  // 5. 恢复所有寄存器
  // 6. 跳回原始指令地址

#if ENABLED(RDOC_X64)
  // x64 shellcode（改进版）:
  // 保存所有易失性寄存器，调用 LoadLibraryW，设置标志，
  // 调用 SuspendThread(GetCurrentThread()) 挂起自身，
  // 恢复寄存器后跳回原始 IP。
  //
  // 当外部检测到标志位为 1 后，调用 ResumeThread 恢复线程，
  // 线程从 SuspendThread 返回后继续执行恢复寄存器和跳回的代码。

  // 预估 shellcode 大小（宽裕估计）
  const size_t shellcodeMaxSize = 256;
  const size_t flagOffset = shellcodeMaxSize + dllPathSize;
  const size_t flagOffsetAligned = (flagOffset + 7) & ~(size_t)7;
  const size_t totalSize = flagOffsetAligned + sizeof(DWORD);

  void *remoteMem =
      VirtualAllocEx(hProcess, NULL, totalSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  if(!remoteMem)
  {
    RDCERR("SetThreadContext injection: couldn't allocate remote memory: %u", GetLastError());
    CloseHandle(hThread);
    return false;
  }

  BYTE *shellcode = new BYTE[totalSize];
  memset(shellcode, 0, totalSize);

  size_t offset = 0;
  BYTE *sc = shellcode;

  DWORD64 dllPathAddr = (DWORD64)remoteMem + shellcodeMaxSize;
  DWORD64 flagAddr = (DWORD64)remoteMem + flagOffsetAligned;

  // === 保存易失性寄存器 ===
  // push rax
  sc[offset++] = 0x50;
  // push rcx
  sc[offset++] = 0x51;
  // push rdx
  sc[offset++] = 0x52;
  // push r8
  sc[offset++] = 0x41; sc[offset++] = 0x50;
  // push r9
  sc[offset++] = 0x41; sc[offset++] = 0x51;
  // push r10
  sc[offset++] = 0x41; sc[offset++] = 0x52;
  // push r11
  sc[offset++] = 0x41; sc[offset++] = 0x53;

  // === 调用 LoadLibraryW ===
  // sub rsp, 0x28 (影子空间 + 对齐)
  sc[offset++] = 0x48; sc[offset++] = 0x83; sc[offset++] = 0xEC; sc[offset++] = 0x28;

  // mov rcx, <dllPathAddr>
  sc[offset++] = 0x48; sc[offset++] = 0xB9;
  memcpy(&sc[offset], &dllPathAddr, 8); offset += 8;

  // mov rax, <LoadLibraryW>
  DWORD64 loadLibAddr = (DWORD64)pLoadLibraryW;
  sc[offset++] = 0x48; sc[offset++] = 0xB8;
  memcpy(&sc[offset], &loadLibAddr, 8); offset += 8;

  // call rax
  sc[offset++] = 0xFF; sc[offset++] = 0xD0;

  // add rsp, 0x28
  sc[offset++] = 0x48; sc[offset++] = 0x83; sc[offset++] = 0xC4; sc[offset++] = 0x28;

  // === 设置完成标志 ===
  // mov rax, <flagAddr>
  sc[offset++] = 0x48; sc[offset++] = 0xB8;
  memcpy(&sc[offset], &flagAddr, 8); offset += 8;

  // mov dword ptr [rax], 1
  sc[offset++] = 0xC7; sc[offset++] = 0x00;
  DWORD one = 1;
  memcpy(&sc[offset], &one, 4); offset += 4;

  // === 调用 SuspendThread(GetCurrentThread()) 挂起自身 ===
  // sub rsp, 0x28
  sc[offset++] = 0x48; sc[offset++] = 0x83; sc[offset++] = 0xEC; sc[offset++] = 0x28;

  // mov rax, <GetCurrentThread>
  DWORD64 getCurrentThreadAddr = (DWORD64)pGetCurrentThread;
  sc[offset++] = 0x48; sc[offset++] = 0xB8;
  memcpy(&sc[offset], &getCurrentThreadAddr, 8); offset += 8;

  // call rax  ; rax = GetCurrentThread() 返回伪句柄
  sc[offset++] = 0xFF; sc[offset++] = 0xD0;

  // mov rcx, rax  ; SuspendThread 的参数
  sc[offset++] = 0x48; sc[offset++] = 0x89; sc[offset++] = 0xC1;

  // mov rax, <SuspendThread>
  DWORD64 suspendThreadAddr = (DWORD64)pSuspendThread;
  sc[offset++] = 0x48; sc[offset++] = 0xB8;
  memcpy(&sc[offset], &suspendThreadAddr, 8); offset += 8;

  // call rax  ; SuspendThread(GetCurrentThread()) - 线程在此挂起
  sc[offset++] = 0xFF; sc[offset++] = 0xD0;

  // add rsp, 0x28
  sc[offset++] = 0x48; sc[offset++] = 0x83; sc[offset++] = 0xC4; sc[offset++] = 0x28;

  // === 线程被 ResumeThread 恢复后从这里继续 ===
  // === 恢复易失性寄存器 ===
  // pop r11
  sc[offset++] = 0x41; sc[offset++] = 0x5B;
  // pop r10
  sc[offset++] = 0x41; sc[offset++] = 0x5A;
  // pop r9
  sc[offset++] = 0x41; sc[offset++] = 0x59;
  // pop r8
  sc[offset++] = 0x41; sc[offset++] = 0x58;
  // pop rdx
  sc[offset++] = 0x5A;
  // pop rcx
  sc[offset++] = 0x59;
  // pop rax
  sc[offset++] = 0x58;

  // === 跳回原始 IP ===
  // 使用 push + ret 的方式跳转，避免破坏任何寄存器
  // push <origIP_low32>  ; 先压入低32位（会被符号扩展）
  // mov dword ptr [rsp+4], <origIP_high32>  ; 修正高32位
  // ret
  DWORD origIP_low = (DWORD)(origIP & 0xFFFFFFFF);
  DWORD origIP_high = (DWORD)(origIP >> 32);

  // push imm32 (低32位，会被符号扩展到64位)
  sc[offset++] = 0x68;
  memcpy(&sc[offset], &origIP_low, 4); offset += 4;

  // mov dword ptr [rsp+4], imm32 (修正高32位)
  sc[offset++] = 0xC7; sc[offset++] = 0x44; sc[offset++] = 0x24; sc[offset++] = 0x04;
  memcpy(&sc[offset], &origIP_high, 4); offset += 4;

  // ret
  sc[offset++] = 0xC3;

  RDCASSERT(offset <= shellcodeMaxSize, offset, shellcodeMaxSize);

  // 复制 DLL 路径到 shellcode 代码之后
  memcpy(&sc[shellcodeMaxSize], dllPath, dllPathSize);

  void *remoteFlagAddr = (BYTE *)remoteMem + flagOffsetAligned;

#else
  // x86 shellcode（改进版）:
  // 保存所有寄存器，调用 LoadLibraryW，设置标志，
  // 调用 SuspendThread(GetCurrentThread()) 挂起自身，
  // 恢复寄存器后跳回原始 IP。

  const size_t shellcodeMaxSize = 128;
  const size_t flagOffset = shellcodeMaxSize + dllPathSize;
  const size_t flagOffsetAligned = (flagOffset + 3) & ~(size_t)3;
  const size_t totalSize = flagOffsetAligned + sizeof(DWORD);

  void *remoteMem =
      VirtualAllocEx(hProcess, NULL, totalSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  if(!remoteMem)
  {
    RDCERR("SetThreadContext injection: couldn't allocate remote memory: %u", GetLastError());
    CloseHandle(hThread);
    return false;
  }

  BYTE *shellcode = new BYTE[totalSize];
  memset(shellcode, 0, totalSize);

  size_t offset = 0;
  BYTE *sc = shellcode;

  DWORD dllPathAddr = (DWORD)((uintptr_t)remoteMem + shellcodeMaxSize);
  DWORD flagAddr = (DWORD)((uintptr_t)remoteMem + flagOffsetAligned);

  // === 保存所有通用寄存器 ===
  // pushad
  sc[offset++] = 0x60;
  // pushfd
  sc[offset++] = 0x9C;

  // === 调用 LoadLibraryW ===
  // push <dllPathAddr>
  sc[offset++] = 0x68;
  memcpy(&sc[offset], &dllPathAddr, 4); offset += 4;

  // mov eax, <LoadLibraryW>
  DWORD loadLibAddr = (DWORD)(uintptr_t)pLoadLibraryW;
  sc[offset++] = 0xB8;
  memcpy(&sc[offset], &loadLibAddr, 4); offset += 4;

  // call eax
  sc[offset++] = 0xFF; sc[offset++] = 0xD0;

  // === 设置完成标志 ===
  // mov dword ptr [<flagAddr>], 1
  sc[offset++] = 0xC7; sc[offset++] = 0x05;
  memcpy(&sc[offset], &flagAddr, 4); offset += 4;
  DWORD one = 1;
  memcpy(&sc[offset], &one, 4); offset += 4;

  // === 调用 SuspendThread(GetCurrentThread()) 挂起自身 ===
  // call GetCurrentThread
  DWORD getCurrentThreadAddrX86 = (DWORD)(uintptr_t)pGetCurrentThread;
  sc[offset++] = 0xB8;
  memcpy(&sc[offset], &getCurrentThreadAddrX86, 4); offset += 4;
  sc[offset++] = 0xFF; sc[offset++] = 0xD0;

  // push eax  ; GetCurrentThread() 返回值作为 SuspendThread 参数
  sc[offset++] = 0x50;

  // call SuspendThread
  DWORD suspendThreadAddrX86 = (DWORD)(uintptr_t)pSuspendThread;
  sc[offset++] = 0xB8;
  memcpy(&sc[offset], &suspendThreadAddrX86, 4); offset += 4;
  sc[offset++] = 0xFF; sc[offset++] = 0xD0;

  // === 线程被 ResumeThread 恢复后从这里继续 ===
  // === 恢复所有寄存器 ===
  // popfd
  sc[offset++] = 0x9D;
  // popad
  sc[offset++] = 0x61;

  // === 跳回原始 IP ===
  // push <origIP> + ret（不破坏任何寄存器）
  sc[offset++] = 0x68;
  memcpy(&sc[offset], &origIP, 4); offset += 4;
  sc[offset++] = 0xC3;

  RDCASSERT(offset <= shellcodeMaxSize, offset, shellcodeMaxSize);

  // 复制 DLL 路径到 shellcode 代码之后
  memcpy(&sc[shellcodeMaxSize], dllPath, dllPathSize);

  void *remoteFlagAddr = (BYTE *)remoteMem + flagOffsetAligned;

#endif

  // 写入 shellcode 到远程进程
  BOOL success = WriteProcessMemory(hProcess, remoteMem, shellcode, totalSize, NULL);
  delete[] shellcode;

  if(!success)
  {
    RDCERR("SetThreadContext injection: couldn't write shellcode to remote memory: %u",
           GetLastError());
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hThread);
    return false;
  }

  // 修改线程上下文，将指令指针指向 shellcode
#if ENABLED(RDOC_X64)
  ctx.Rip = (DWORD64)remoteMem;
#else
  ctx.Eip = (DWORD)(uintptr_t)remoteMem;
#endif

  if(!SetThreadContext(hThread, &ctx))
  {
    RDCERR("SetThreadContext injection: SetThreadContext failed: %u", GetLastError());
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hThread);
    return false;
  }

  RDCLOG("SetThreadContext injection: successfully hijacked thread, shellcode at %p, "
         "original IP at %p, flag at %p",
         remoteMem, (void *)(uintptr_t)origIP, remoteFlagAddr);

  // 恢复线程执行 shellcode。
  // FindMainThread 中做了一次 SuspendThread，这里需要两次 ResumeThread：
  // 第一次抵消 FindMainThread 的挂起，第二次抵消 CREATE_SUSPENDED 的挂起。
  // 但如果是 InjectIntoProcess（非 CREATE_SUSPENDED 场景），
  // 线程可能只有一层挂起，所以我们只 Resume 到线程真正运行为止。
  DWORD suspCount;
  do {
    suspCount = ResumeThread(hThread);
  } while(suspCount > 1);

  // 等待 DLL 加载完成。
  // shellcode 在 LoadLibraryW 返回后会将标志位设为 1，然后调用 SuspendThread 挂起自身。
  // 我们轮询读取标志位来判断 LoadLibraryW 是否已完成。
  const DWORD timeout = 30000;    // 30 秒超时（DLL 加载可能较慢）
  DWORD elapsed = 0;
  bool loaded = false;

  while(elapsed < timeout)
  {
    Sleep(50);
    elapsed += 50;

    // 从远程进程读取标志位
    DWORD flagValue = 0;
    SIZE_T bytesRead = 0;
    if(ReadProcessMemory(hProcess, remoteFlagAddr, &flagValue, sizeof(flagValue), &bytesRead))
    {
      if(flagValue == 1)
      {
        loaded = true;
        RDCLOG("SetThreadContext injection: LoadLibraryW completed (waited %u ms)", elapsed);
        break;
      }
    }
  }

  if(!loaded)
  {
    RDCWARN("SetThreadContext injection: timed out waiting for LoadLibraryW to complete (%u ms)",
            timeout);
  }

  // shellcode 在设置标志后调用了 SuspendThread(GetCurrentThread()) 挂起自身。
  // 线程现在处于挂起状态，等待外部 ResumeThread。
  //
  // 我们需要通过 SetThreadContext 恢复线程的原始上下文（IP 和寄存器），
  // 这样后续的 InjectFunctionCall 获取到的上下文是正确的原始状态，
  // 而不是 shellcode 中 SuspendThread 返回后的位置。
  if(loaded)
  {
    // 线程已经挂起（shellcode 自己挂起的），可以直接操作上下文
    CONTEXT restoreCtx = {};
    restoreCtx.ContextFlags = CONTEXT_FULL;
    if(GetThreadContext(hThread, &restoreCtx))
    {
#if ENABLED(RDOC_X64)
      restoreCtx.Rip = origIP;
      restoreCtx.Rcx = origRCX;
      restoreCtx.Rdx = origRDX;
      restoreCtx.R8 = origR8;
      restoreCtx.R9 = origR9;
      restoreCtx.R10 = origR10;
      restoreCtx.R11 = origR11;
      restoreCtx.Rax = origRAX;
      restoreCtx.Rsp = ctx.Rsp;
#else
      restoreCtx.Eip = origIP;
      restoreCtx.Eax = ctx.Eax;
      restoreCtx.Ecx = ctx.Ecx;
      restoreCtx.Edx = ctx.Edx;
      restoreCtx.Ebx = ctx.Ebx;
      restoreCtx.Esp = ctx.Esp;
      restoreCtx.Ebp = ctx.Ebp;
      restoreCtx.Esi = ctx.Esi;
      restoreCtx.Edi = ctx.Edi;
#endif
      SetThreadContext(hThread, &restoreCtx);
    }
  }

  // 注意：不释放 remoteMem，因为进程退出时会自动释放。

  CloseHandle(hThread);
  return loaded;
}

void InjectDLL(HANDLE hProcess, rdcwstr libName)
{
  wchar_t dllPath[MAX_PATH + 1] = {0};
  wcscpy_s(dllPath, libName.c_str());

  DWORD pid = GetProcessId(hProcess);

  // 首先尝试 SetThreadContext 注入方式
  // 这种方式不创建新线程，而是劫持已有的挂起线程来执行 LoadLibraryW，
  // 可以绕过某些反作弊系统对 CreateRemoteThread 的拦截。
  RDCLOG("Attempting SetThreadContext injection for process %u", pid);
  if(InjectDLL_SetThreadContext(hProcess, pid, dllPath, sizeof(dllPath)))
  {
    RDCLOG("SetThreadContext injection succeeded for process %u", pid);
    return;
  }

  // SetThreadContext 方式失败，回退到 CreateRemoteThread 方式
  RDCWARN("SetThreadContext injection failed, falling back to CreateRemoteThread for process %u",
          pid);
  if(!InjectDLL_CreateRemoteThread(hProcess, dllPath, sizeof(dllPath)))
  {
    RDCERR("All injection methods failed for process %u", pid);
  }
}

uintptr_t FindRemoteDLL(DWORD pid, rdcstr libName)
{
  HANDLE hModuleSnap = INVALID_HANDLE_VALUE;

  rdcwstr wlibName = StringFormat::UTF82Wide(strlower(libName));

  // up to 10 retries
  for(int i = 0; i < 10; i++)
  {
    hModuleSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);

    if(hModuleSnap == INVALID_HANDLE_VALUE)
    {
      DWORD err = GetLastError();

      RDCWARN("CreateToolhelp32Snapshot(%u) -> 0x%08x", pid, err);

      // retry if error is ERROR_BAD_LENGTH
      if(err == ERROR_BAD_LENGTH)
        continue;
    }

    // didn't retry, or succeeded
    break;
  }

  if(hModuleSnap == INVALID_HANDLE_VALUE)
  {
    RDCERR("Couldn't create toolhelp dump of modules in process %u", pid);
    return 0;
  }

  MODULEENTRY32 me32;
  RDCEraseEl(me32);
  me32.dwSize = sizeof(MODULEENTRY32);

  BOOL success = Module32First(hModuleSnap, &me32);

  if(success == FALSE)
  {
    DWORD err = GetLastError();

    RDCERR("Couldn't get first module in process %u: 0x%08x", pid, err);
    CloseHandle(hModuleSnap);
    return 0;
  }

  uintptr_t ret = 0;

  int numModules = 0;

  do
  {
    wchar_t modnameLower[MAX_MODULE_NAME32 + 1];
    RDCEraseEl(modnameLower);
    wcsncpy_s(modnameLower, me32.szModule, MAX_MODULE_NAME32);

    wchar_t *wc = &modnameLower[0];
    while(*wc)
    {
      *wc = towlower(*wc);
      wc++;
    }

    numModules++;

    if(wcsstr(modnameLower, wlibName.c_str()) == modnameLower)
    {
      ret = (uintptr_t)me32.modBaseAddr;
    }
  } while(ret == 0 && Module32Next(hModuleSnap, &me32));

  if(ret == 0)
  {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);

    DWORD exitCode = 0;

    if(h)
      GetExitCodeProcess(h, &exitCode);

    if(h == NULL || exitCode != STILL_ACTIVE)
    {
      RDCERR(
          "Error injecting into remote process with PID %u which is no longer available.\n"
          "Possibly the process has crashed during early startup, or is missing DLLs to run?",
          pid);
    }
    else
    {
      RDCERR("Couldn't find module '%s' among %d modules", libName.c_str(), numModules);
    }

    if(h)
      CloseHandle(h);
  }

  CloseHandle(hModuleSnap);

  return ret;
}

// 通过 SetThreadContext 方式调用远程函数（替代 CreateRemoteThread）
// 原理：劫持目标进程的挂起线程，让其执行 shellcode 来调用指定函数，
// 函数执行完毕后 shellcode 会挂起自身，等待外部恢复。
void InjectFunctionCall(HANDLE hProcess, uintptr_t renderdoc_remote, const char *funcName,
                        void *data, const size_t dataLen)
{
  if(dataLen == 0)
  {
    RDCERR("Invalid function call injection attempt");
    return;
  }

  RDCDEBUG("Injecting call to %s", funcName);

  HMODULE renderdoc_local = GetModuleHandleA(STRINGIZE(RDOC_BASE_NAME) ".dll");

  uintptr_t func_local = (uintptr_t)GetProcAddress(renderdoc_local, funcName);

  // we've found SetCaptureOptions in our local instance of the module, now calculate the offset and
  // so get the function
  // in the remote module (which might be loaded at a different base address
  uintptr_t func_remote = func_local + renderdoc_remote - (uintptr_t)renderdoc_local;

  DWORD pid = GetProcessId(hProcess);

  // 查找主线程（FindMainThread 会确保线程处于挂起状态）
  HANDLE hThread = FindMainThread(pid);
  if(!hThread)
  {
    RDCERR("InjectFunctionCall: couldn't find main thread for process %u, "
           "falling back to CreateRemoteThread", pid);
    // 回退到 CreateRemoteThread
    void *remoteMem = VirtualAllocEx(hProcess, NULL, dataLen, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    SIZE_T numWritten;
    WriteProcessMemory(hProcess, remoteMem, data, dataLen, &numWritten);
    HANDLE hRemoteThread =
        CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)func_remote, remoteMem, 0, NULL);
    if(hRemoteThread)
    {
      WaitForSingleObject(hRemoteThread, INFINITE);
      ReadProcessMemory(hProcess, remoteMem, data, dataLen, &numWritten);
      CloseHandle(hRemoteThread);
    }
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    return;
  }

  static HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
  void *pGetCurrentThread = (void *)GetProcAddress(kernel32, "GetCurrentThread");
  void *pSuspendThread = (void *)GetProcAddress(kernel32, "SuspendThread");

  // 获取线程上下文
  CONTEXT ctx = {};
  ctx.ContextFlags = CONTEXT_FULL;
  if(!GetThreadContext(hThread, &ctx))
  {
    RDCERR("InjectFunctionCall: GetThreadContext failed: %u", GetLastError());
    ResumeThread(hThread);    // 抵消 FindMainThread 的挂起
    CloseHandle(hThread);
    return;
  }

#if ENABLED(RDOC_X64)
  DWORD64 origIP = ctx.Rip;
#else
  DWORD origIP = ctx.Eip;
#endif

  // 写入函数参数数据到远程进程
  void *remoteData = VirtualAllocEx(hProcess, NULL, dataLen, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  if(!remoteData)
  {
    RDCERR("InjectFunctionCall: couldn't allocate remote memory for data: %u", GetLastError());
    ResumeThread(hThread);
    CloseHandle(hThread);
    return;
  }
  SIZE_T numWritten;
  WriteProcessMemory(hProcess, remoteData, data, dataLen, &numWritten);

#if ENABLED(RDOC_X64)
  // x64 shellcode:
  // 保存易失性寄存器 -> 调用 func_remote(remoteData) -> 设置完成标志 ->
  // SuspendThread(GetCurrentThread()) -> 恢复寄存器 -> 跳回原始 IP
  const size_t shellcodeMaxSize = 256;
  const size_t flagOffsetAligned = (shellcodeMaxSize + 7) & ~(size_t)7;
  const size_t totalSize = flagOffsetAligned + sizeof(DWORD);

  void *remoteMem = VirtualAllocEx(hProcess, NULL, totalSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  if(!remoteMem)
  {
    RDCERR("InjectFunctionCall: couldn't allocate remote memory for shellcode: %u", GetLastError());
    VirtualFreeEx(hProcess, remoteData, 0, MEM_RELEASE);
    ResumeThread(hThread);
    CloseHandle(hThread);
    return;
  }

  BYTE *shellcode = new BYTE[totalSize];
  memset(shellcode, 0, totalSize);

  size_t offset = 0;
  BYTE *sc = shellcode;

  DWORD64 flagAddr = (DWORD64)remoteMem + flagOffsetAligned;

  // 保存易失性寄存器
  sc[offset++] = 0x50;    // push rax
  sc[offset++] = 0x51;    // push rcx
  sc[offset++] = 0x52;    // push rdx
  sc[offset++] = 0x41; sc[offset++] = 0x50;    // push r8
  sc[offset++] = 0x41; sc[offset++] = 0x51;    // push r9
  sc[offset++] = 0x41; sc[offset++] = 0x52;    // push r10
  sc[offset++] = 0x41; sc[offset++] = 0x53;    // push r11

  // sub rsp, 0x28 (影子空间)
  sc[offset++] = 0x48; sc[offset++] = 0x83; sc[offset++] = 0xEC; sc[offset++] = 0x28;

  // mov rcx, <remoteData>  ; 函数参数
  DWORD64 remoteDataAddr = (DWORD64)remoteData;
  sc[offset++] = 0x48; sc[offset++] = 0xB9;
  memcpy(&sc[offset], &remoteDataAddr, 8); offset += 8;

  // mov rax, <func_remote>
  DWORD64 funcAddr = (DWORD64)func_remote;
  sc[offset++] = 0x48; sc[offset++] = 0xB8;
  memcpy(&sc[offset], &funcAddr, 8); offset += 8;

  // call rax
  sc[offset++] = 0xFF; sc[offset++] = 0xD0;

  // add rsp, 0x28
  sc[offset++] = 0x48; sc[offset++] = 0x83; sc[offset++] = 0xC4; sc[offset++] = 0x28;

  // 设置完成标志
  // mov rax, <flagAddr>
  sc[offset++] = 0x48; sc[offset++] = 0xB8;
  memcpy(&sc[offset], &flagAddr, 8); offset += 8;
  // mov dword ptr [rax], 1
  sc[offset++] = 0xC7; sc[offset++] = 0x00;
  DWORD one = 1;
  memcpy(&sc[offset], &one, 4); offset += 4;

  // SuspendThread(GetCurrentThread())
  sc[offset++] = 0x48; sc[offset++] = 0x83; sc[offset++] = 0xEC; sc[offset++] = 0x28;
  DWORD64 getCurrentThreadAddr = (DWORD64)pGetCurrentThread;
  sc[offset++] = 0x48; sc[offset++] = 0xB8;
  memcpy(&sc[offset], &getCurrentThreadAddr, 8); offset += 8;
  sc[offset++] = 0xFF; sc[offset++] = 0xD0;
  sc[offset++] = 0x48; sc[offset++] = 0x89; sc[offset++] = 0xC1;    // mov rcx, rax
  DWORD64 suspendThreadAddr = (DWORD64)pSuspendThread;
  sc[offset++] = 0x48; sc[offset++] = 0xB8;
  memcpy(&sc[offset], &suspendThreadAddr, 8); offset += 8;
  sc[offset++] = 0xFF; sc[offset++] = 0xD0;
  sc[offset++] = 0x48; sc[offset++] = 0x83; sc[offset++] = 0xC4; sc[offset++] = 0x28;

  // 恢复寄存器
  sc[offset++] = 0x41; sc[offset++] = 0x5B;    // pop r11
  sc[offset++] = 0x41; sc[offset++] = 0x5A;    // pop r10
  sc[offset++] = 0x41; sc[offset++] = 0x59;    // pop r9
  sc[offset++] = 0x41; sc[offset++] = 0x58;    // pop r8
  sc[offset++] = 0x5A;    // pop rdx
  sc[offset++] = 0x59;    // pop rcx
  sc[offset++] = 0x58;    // pop rax

  // 跳回原始 IP（push + ret 方式，不破坏寄存器）
  DWORD origIP_low = (DWORD)(origIP & 0xFFFFFFFF);
  DWORD origIP_high = (DWORD)(origIP >> 32);
  sc[offset++] = 0x68;
  memcpy(&sc[offset], &origIP_low, 4); offset += 4;
  sc[offset++] = 0xC7; sc[offset++] = 0x44; sc[offset++] = 0x24; sc[offset++] = 0x04;
  memcpy(&sc[offset], &origIP_high, 4); offset += 4;
  sc[offset++] = 0xC3;

  RDCASSERT(offset <= shellcodeMaxSize, offset, shellcodeMaxSize);

  void *remoteFlagAddr = (BYTE *)remoteMem + flagOffsetAligned;

#else
  // x86 shellcode
  const size_t shellcodeMaxSize = 128;
  const size_t flagOffsetAligned = (shellcodeMaxSize + 3) & ~(size_t)3;
  const size_t totalSize = flagOffsetAligned + sizeof(DWORD);

  void *remoteMem = VirtualAllocEx(hProcess, NULL, totalSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  if(!remoteMem)
  {
    RDCERR("InjectFunctionCall: couldn't allocate remote memory for shellcode: %u", GetLastError());
    VirtualFreeEx(hProcess, remoteData, 0, MEM_RELEASE);
    ResumeThread(hThread);
    CloseHandle(hThread);
    return;
  }

  BYTE *shellcode = new BYTE[totalSize];
  memset(shellcode, 0, totalSize);

  size_t offset = 0;
  BYTE *sc = shellcode;

  DWORD flagAddr = (DWORD)((uintptr_t)remoteMem + flagOffsetAligned);

  // pushad + pushfd
  sc[offset++] = 0x60;
  sc[offset++] = 0x9C;

  // push <remoteData>  ; 函数参数
  DWORD remoteDataAddrX86 = (DWORD)(uintptr_t)remoteData;
  sc[offset++] = 0x68;
  memcpy(&sc[offset], &remoteDataAddrX86, 4); offset += 4;

  // mov eax, <func_remote>
  DWORD funcAddrX86 = (DWORD)(uintptr_t)func_remote;
  sc[offset++] = 0xB8;
  memcpy(&sc[offset], &funcAddrX86, 4); offset += 4;

  // call eax
  sc[offset++] = 0xFF; sc[offset++] = 0xD0;

  // 设置完成标志
  sc[offset++] = 0xC7; sc[offset++] = 0x05;
  memcpy(&sc[offset], &flagAddr, 4); offset += 4;
  DWORD one = 1;
  memcpy(&sc[offset], &one, 4); offset += 4;

  // SuspendThread(GetCurrentThread())
  DWORD getCurrentThreadAddrX86 = (DWORD)(uintptr_t)pGetCurrentThread;
  sc[offset++] = 0xB8;
  memcpy(&sc[offset], &getCurrentThreadAddrX86, 4); offset += 4;
  sc[offset++] = 0xFF; sc[offset++] = 0xD0;
  sc[offset++] = 0x50;    // push eax
  DWORD suspendThreadAddrX86 = (DWORD)(uintptr_t)pSuspendThread;
  sc[offset++] = 0xB8;
  memcpy(&sc[offset], &suspendThreadAddrX86, 4); offset += 4;
  sc[offset++] = 0xFF; sc[offset++] = 0xD0;

  // popfd + popad
  sc[offset++] = 0x9D;
  sc[offset++] = 0x61;

  // 跳回原始 IP
  sc[offset++] = 0x68;
  memcpy(&sc[offset], &origIP, 4); offset += 4;
  sc[offset++] = 0xC3;

  RDCASSERT(offset <= shellcodeMaxSize, offset, shellcodeMaxSize);

  void *remoteFlagAddr = (BYTE *)remoteMem + flagOffsetAligned;

#endif

  // 写入 shellcode
  BOOL success = WriteProcessMemory(hProcess, remoteMem, shellcode, totalSize, NULL);
  delete[] shellcode;

  if(!success)
  {
    RDCERR("InjectFunctionCall: couldn't write shellcode: %u", GetLastError());
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    VirtualFreeEx(hProcess, remoteData, 0, MEM_RELEASE);
    ResumeThread(hThread);
    CloseHandle(hThread);
    return;
  }

  // 修改线程上下文
#if ENABLED(RDOC_X64)
  ctx.Rip = (DWORD64)remoteMem;
#else
  ctx.Eip = (DWORD)(uintptr_t)remoteMem;
#endif

  if(!SetThreadContext(hThread, &ctx))
  {
    RDCERR("InjectFunctionCall: SetThreadContext failed: %u", GetLastError());
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    VirtualFreeEx(hProcess, remoteData, 0, MEM_RELEASE);
    ResumeThread(hThread);
    CloseHandle(hThread);
    return;
  }

  // 恢复线程执行 shellcode。
  // 线程当前的挂起计数可能 > 1（来自之前的 SuspendThread + FindMainThread 的 SuspendThread），
  // 需要循环 ResumeThread 直到线程真正运行。
  {
    DWORD suspCount;
    do {
      suspCount = ResumeThread(hThread);
    } while(suspCount > 1);
  }

  // 等待函数调用完成
  const DWORD timeout = 30000;
  DWORD elapsed = 0;
  bool completed = false;

  while(elapsed < timeout)
  {
    Sleep(10);
    elapsed += 10;

    DWORD flagValue = 0;
    SIZE_T bytesRead = 0;
    if(ReadProcessMemory(hProcess, remoteFlagAddr, &flagValue, sizeof(flagValue), &bytesRead))
    {
      if(flagValue == 1)
      {
        completed = true;
        break;
      }
    }
  }

  if(!completed)
  {
    RDCWARN("InjectFunctionCall: timed out waiting for %s to complete", funcName);
  }

  // 读回函数执行后的数据
  ReadProcessMemory(hProcess, remoteData, data, dataLen, &numWritten);

  // shellcode 已经通过 SuspendThread(GetCurrentThread()) 挂起了自身。
  // 我们需要恢复线程让它继续执行恢复寄存器和跳回原始 IP 的代码，
  // 然后再次等待它到达原始 IP 后挂起（为下一次 InjectFunctionCall 做准备）。
  // 但由于 shellcode 跳回原始 IP 后线程就自由运行了，
  // 我们不能在这里再次挂起（会有竞态）。
  // 所以我们不恢复线程，让它保持挂起状态。
  // 下一次 InjectFunctionCall 或最终的 ResumeThread 会处理恢复。
  //
  // 但是这样线程的 IP 还在 shellcode 中（SuspendThread 返回后的位置），
  // 我们需要手动设置线程上下文回到原始 IP。
  CONTEXT restoreCtx = {};
  restoreCtx.ContextFlags = CONTEXT_FULL;
  GetThreadContext(hThread, &restoreCtx);

  // 恢复原始上下文（在 shellcode 开始前保存的）
  // 由于 shellcode 已经挂起，我们直接设置回原始 IP
#if ENABLED(RDOC_X64)
  restoreCtx.Rip = origIP;
  // 恢复原始上下文中保存的寄存器值
  restoreCtx.Rax = ctx.Rax;
  restoreCtx.Rcx = ctx.Rcx;
  restoreCtx.Rdx = ctx.Rdx;
  restoreCtx.R8 = ctx.R8;
  restoreCtx.R9 = ctx.R9;
  restoreCtx.R10 = ctx.R10;
  restoreCtx.R11 = ctx.R11;
  restoreCtx.Rsp = ctx.Rsp;    // 恢复原始栈指针
#else
  restoreCtx.Eip = origIP;
  restoreCtx.Eax = ctx.Eax;
  restoreCtx.Ecx = ctx.Ecx;
  restoreCtx.Edx = ctx.Edx;
  restoreCtx.Ebx = ctx.Ebx;
  restoreCtx.Esp = ctx.Esp;
  restoreCtx.Ebp = ctx.Ebp;
  restoreCtx.Esi = ctx.Esi;
  restoreCtx.Edi = ctx.Edi;
#endif
  SetThreadContext(hThread, &restoreCtx);

  // 清理 shellcode 内存（线程已经不在 shellcode 中了）
  VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
  VirtualFreeEx(hProcess, remoteData, 0, MEM_RELEASE);

  // 线程保持挂起状态，等待下一次操作或最终恢复
  CloseHandle(hThread);
}

static PROCESS_INFORMATION RunProcess(const rdcstr &app, const rdcstr &workingDir,
                                      const rdcstr &cmdLine,
                                      const rdcarray<EnvironmentModification> &env, bool internal,
                                      HANDLE *phChildStdOutput_Rd, HANDLE *phChildStdError_Rd)
{
  PROCESS_INFORMATION pi;
  STARTUPINFO si;
  SECURITY_ATTRIBUTES pSec;
  SECURITY_ATTRIBUTES tSec;

  RDCEraseEl(pi);
  RDCEraseEl(si);
  RDCEraseEl(pSec);
  RDCEraseEl(tSec);

  si.cb = sizeof(si);

  pSec.nLength = sizeof(pSec);
  tSec.nLength = sizeof(tSec);

  rdcwstr workdir = L"";

  if(!workingDir.empty())
    workdir = StringFormat::UTF82Wide(workingDir);
  else
    workdir = StringFormat::UTF82Wide(get_dirname(app));

  wchar_t *paramsAlloc = NULL;

  rdcwstr wapp = StringFormat::UTF82Wide(app);

  // CreateProcessW can modify the params, need space.
  size_t len = wapp.length() + 10;

  rdcwstr wcmd = L"";

  if(!cmdLine.empty())
  {
    wcmd = StringFormat::UTF82Wide(cmdLine);
    len += wcmd.length();
  }

  paramsAlloc = new wchar_t[len];

  RDCEraseMem(paramsAlloc, len * sizeof(wchar_t));

  wcscpy_s(paramsAlloc, len, L"\"");
  wcscat_s(paramsAlloc, len, wapp.c_str());
  wcscat_s(paramsAlloc, len, L"\"");

  if(!cmdLine.empty())
  {
    wcscat_s(paramsAlloc, len, L" ");
    wcscat_s(paramsAlloc, len, wcmd.c_str());
  }

  bool inheritHandles = false;

  HANDLE hChildStdOutput_Wr = 0, hChildStdError_Wr = 0;
  if(phChildStdOutput_Rd)
  {
    RDCASSERT(phChildStdError_Rd);

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if(!CreatePipe(phChildStdOutput_Rd, &hChildStdOutput_Wr, &sa, 0))
      RDCERR("Could not create pipe to read stdout");
    if(!SetHandleInformation(*phChildStdOutput_Rd, HANDLE_FLAG_INHERIT, 0))
      RDCERR("Could not set pipe handle information");

    if(!CreatePipe(phChildStdError_Rd, &hChildStdError_Wr, &sa, 0))
      RDCERR("Could not create pipe to read stdout");
    if(!SetHandleInformation(*phChildStdError_Rd, HANDLE_FLAG_INHERIT, 0))
      RDCERR("Could not set pipe handle information");

    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdOutput = hChildStdOutput_Wr;
    si.hStdError = hChildStdError_Wr;

    // Need to inherit handles in CreateProcess for ReadFile to read stdout
    inheritHandles = true;
  }

  // if it's a utility launch, hide the command prompt window from showing
  if(phChildStdOutput_Rd || internal)
    si.dwFlags |= STARTF_USESHOWWINDOW;

  if(!internal)
    RDCLOG("Running process %s", app.c_str());

  // turn environment string to a UTF-8 map
  std::wstring envString;

  if(!env.empty())
  {
    LPWCH envStrings = GetEnvironmentStringsW();
    EnvMap envValues = EnvStringToEnvMap(envStrings);
    FreeEnvironmentStringsW(envStrings);

    ApplyEnvModifications(envValues, env, false);

    for(auto it = envValues.begin(); it != envValues.end(); ++it)
    {
      envString += StringFormat::UTF82Wide(it->first).c_str();
      envString += L"=";
      envString += StringFormat::UTF82Wide(it->second).c_str();
      envString.push_back(0);
    }
  }

  BOOL retValue = CreateProcessW(
      NULL, paramsAlloc, &pSec, &tSec, inheritHandles, CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
      envString.empty() ? NULL : (void *)envString.data(), workdir.c_str(), &si, &pi);

  DWORD err = GetLastError();

  if(phChildStdOutput_Rd)
  {
    CloseHandle(hChildStdOutput_Wr);
    CloseHandle(hChildStdError_Wr);
  }

  SAFE_DELETE_ARRAY(paramsAlloc);

  if(!retValue)
  {
    if(!internal)
      RDCWARN("Process %s could not be loaded (error %d).", app.c_str(), err);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    RDCEraseEl(pi);
  }

  return pi;
}

rdcpair<RDResult, uint32_t> Process::InjectIntoProcess(uint32_t pid,
                                                       const rdcarray<EnvironmentModification> &env,
                                                       const rdcstr &capturefile,
                                                       const CaptureOptions &opts, bool waitForExit)
{
  rdcwstr wcapturefile = StringFormat::UTF82Wide(capturefile);

  HANDLE hProcess =
      OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                      PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE,
                  FALSE, pid);

  if(opts.delayForDebugger > 0)
  {
    RDCDEBUG("Waiting for debugger attach to %lu", pid);
    uint32_t timeout = 0;

    BOOL debuggerAttached = FALSE;

    while(!debuggerAttached)
    {
      CheckRemoteDebuggerPresent(hProcess, &debuggerAttached);

      Sleep(10);
      timeout += 10;

      if(timeout > opts.delayForDebugger * 1000)
        break;
    }

    if(debuggerAttached)
      RDCDEBUG("Debugger attach detected after %.2f s", float(timeout) / 1000.0f);
    else
      RDCDEBUG("Timed out waiting for debugger, gave up after %u s", opts.delayForDebugger);
  }

  RDCLOG("Injecting renderdoc into process %lu", pid);

  wchar_t renderdocPath[MAX_PATH] = {0};
  GetModuleFileNameW(GetModuleHandleA(STRINGIZE(RDOC_BASE_NAME) ".dll"), &renderdocPath[0],
                                      MAX_PATH - 1);

  wchar_t renderdocPathLower[MAX_PATH] = {0};
  memcpy(renderdocPathLower, renderdocPath, MAX_PATH * sizeof(wchar_t));
  for(size_t i = 0; i < MAX_PATH && renderdocPathLower[i]; i++)
  {
    // lowercase
    if(renderdocPathLower[i] >= 'A' && renderdocPathLower[i] <= 'Z')
      renderdocPathLower[i] = 'a' + char(renderdocPathLower[i] - 'A');

    // normalise paths
    if(renderdocPathLower[i] == '/')
      renderdocPathLower[i] = '\\';
  }

  BOOL isWow64 = FALSE;
  BOOL success = IsWow64Process(hProcess, &isWow64);

  if(!success)
  {
    DWORD err = GetLastError();
    RDResult result;
    SET_ERROR_RESULT(result, ResultCode::IncompatibleProcess,
                     "Couldn't determine bitness of process, err: %08x", err);
    CloseHandle(hProcess);
    return {result, 0};
  }

  bool capalt = false;

#if DISABLED(RDOC_X64)
  BOOL selfWow64 = FALSE;

  HANDLE hSelfProcess = GetCurrentProcess();

  // check to see if we're a WoW64 process
  success = IsWow64Process(hSelfProcess, &selfWow64);

  CloseHandle(hSelfProcess);

  if(!success)
  {
    DWORD err = GetLastError();
    RDResult result;
    SET_ERROR_RESULT(result, ResultCode::IncompatibleProcess,
                     "Couldn't determine bitness of self, err: %08x", err);
    CloseHandle(hProcess);
    return {result, 0};
  }

  // we know we're 32-bit, so if the target process is not wow64
  // and we are, it's 64-bit. If we're both not wow64 then we're
  // running on 32-bit windows, and if we're both wow64 then we're
  // both 32-bit on 64-bit windows.
  //
  // We don't support capturing 64-bit programs from a 32-bit install
  // because it's pointless - a 64-bit install will work for all in
  // that case. But we do want to handle the case of:
  // 64-bit renderdoc -> 32-bit program (via 32-bit renderdoccmd)
  //    -> 64-bit program (going back to 64-bit renderdoccmd).
  // so we try to see if we're an x86 invoked renderdoccmd in an
  // otherwise 64-bit install, and 'promote' back to 64-bit.
  if(selfWow64 && !isWow64)
  {
    wchar_t *slash = wcsrchr(renderdocPath, L'\\');

    if(slash && slash > renderdocPath + 4)
    {
      slash -= 4;

      if(slash && !wcsncmp(slash, L"\\x86", 4))
      {
        RDCDEBUG("Promoting back to 64-bit");
        capalt = true;
      }
    }

    // if it looks like we're in the development environment, look for the alternate bitness in the
    // corresponding folder
    if(!capalt)
    {
      const wchar_t *devLocation = wcsstr(renderdocPathLower, L"\\win32\\development\\");
      if(!devLocation)
        devLocation = wcsstr(renderdocPathLower, L"\\win32\\release\\");

      if(devLocation)
      {
        RDCDEBUG("Promoting back to 64-bit");
        capalt = true;
      }
    }

    // if we couldn't promote, then bail out.
    if(!capalt)
    {
      RDCDEBUG("Running from %ls", renderdocPathLower);

      CloseHandle(hProcess);
      RDResult result;
      SET_ERROR_RESULT(result, ResultCode::IncompatibleProcess,
                       "Can't capture 64-bit program with 32-bit build. Please run a "
                       "64-bit build");
      return {result, 0};
    }
  }
#else
  // farm off to alternate bitness renderdoccmd.exe

  // if the target process is 'wow64' that means it's 32-bit.
  capalt = (isWow64 == TRUE);
#endif

  if(capalt)
  {
#if ENABLED(RDOC_X64)
    // if it looks like we're in the development environment, look for the alternate bitness in the
    // corresponding folder
    const wchar_t *devLocation = wcsstr(renderdocPathLower, L"\\x64\\development\\");
    if(devLocation)
    {
      size_t idx = devLocation - renderdocPathLower;

      renderdocPath[idx] = 0;

      wcscat_s(renderdocPath, L"\\Win32\\Development\\gfxdiagcmd.exe");
    }

    if(!devLocation)
    {
      devLocation = wcsstr(renderdocPathLower, L"\\x64\\release\\");

      if(devLocation)
      {
        size_t idx = devLocation - renderdocPathLower;

        renderdocPath[idx] = 0;

        wcscat_s(renderdocPath, L"\\Win32\\Release\\gfxdiagcmd.exe");
      }
    }

    if(!devLocation)
    {
      // look in a subfolder for x86.

      // remove the filename from the path
      wchar_t *slash = wcsrchr(renderdocPath, L'\\');

      if(slash)
        *slash = 0;

      // append path
      wcscat_s(renderdocPath, L"\\x86\\gfxdiagcmd.exe");
    }
#else
    // if it looks like we're in the development environment, look for the alternate bitness in the
    // corresponding folder
    const wchar_t *devLocation = wcsstr(renderdocPathLower, L"\\win32\\development\\");
    if(devLocation)
    {
      size_t idx = devLocation - renderdocPathLower;

      renderdocPath[idx] = 0;

      wcscat_s(renderdocPath, L"\\x64\\Development\\gfxdiagcmd.exe");
    }

    if(!devLocation)
    {
      devLocation = wcsstr(renderdocPathLower, L"\\win32\\release\\");

      if(devLocation)
      {
        size_t idx = devLocation - renderdocPathLower;

        renderdocPath[idx] = 0;

        wcscat_s(renderdocPath, L"\\x64\\Release\\gfxdiagcmd.exe");
      }
    }

    if(!devLocation)
    {
      // look upwards on 32-bit to find the parent renderdoccmd.
      wchar_t *slash = wcsrchr(renderdocPath, L'\\');

      // remove the filename
      if(slash)
        *slash = 0;

      // remove the \\x86
      slash = wcsrchr(renderdocPath, L'\\');

      if(slash)
        *slash = 0;

      // append path
      wcscat_s(renderdocPath, L"\\gfxdiagcmd.exe");
    }
#endif

    PROCESS_INFORMATION pi;
    STARTUPINFO si;
    SECURITY_ATTRIBUTES pSec;
    SECURITY_ATTRIBUTES tSec;

    RDCEraseEl(pi);
    RDCEraseEl(si);
    RDCEraseEl(pSec);
    RDCEraseEl(tSec);

    // hide the console window
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    pSec.nLength = sizeof(pSec);
    tSec.nLength = sizeof(tSec);

    // serialise to string with two chars per byte
    rdcstr optstr = opts.EncodeAsString();

    wchar_t *paramsAlloc = new wchar_t[2048];

    rdcstr debugLogfile = RDCGETLOGFILE();
    rdcwstr wdebugLogfile = StringFormat::UTF82Wide(debugLogfile);

    _snwprintf_s(
        paramsAlloc, 2047, 2047,
        L"\"%ls\" capaltbit --pid=%u --capfile=\"%ls\" --debuglog=\"%ls\" --capopts=\"%hs\"",
        renderdocPath, pid, wcapturefile.c_str(), wdebugLogfile.c_str(), optstr.c_str());

    RDCDEBUG("params %ls", paramsAlloc);

    paramsAlloc[2047] = 0;

    wchar_t *commandLine = paramsAlloc;

    std::wstring cmdWithEnv;

    if(!env.empty())
    {
      cmdWithEnv = paramsAlloc;

      for(const EnvironmentModification &e : env)
      {
        rdcstr name = e.name.trimmed();
        rdcstr value = e.value;

        if(name == "")
          break;

        cmdWithEnv += L" +env-";
        switch(e.mod)
        {
          case EnvMod::Set: cmdWithEnv += L"replace"; break;
          case EnvMod::Append: cmdWithEnv += L"append"; break;
          case EnvMod::Prepend: cmdWithEnv += L"prepend"; break;
        }

        if(e.mod != EnvMod::Set)
        {
          switch(e.sep)
          {
            case EnvSep::Platform: cmdWithEnv += L"-platform"; break;
            case EnvSep::SemiColon: cmdWithEnv += L"-semicolon"; break;
            case EnvSep::Colon: cmdWithEnv += L"-colon"; break;
            case EnvSep::NoSep: break;
          }
        }

        cmdWithEnv += L" ";

        // escape the parameters
        for(size_t it = 0; it < name.size(); it++)
        {
          if(name[it] == '"')
          {
            name.insert(it, '\\');
            it++;
          }
        }

        for(size_t it = 0; it < value.size(); it++)
        {
          if(value[it] == '"')
          {
            value.insert(it, '\\');
            it++;
          }
        }

        if(name.back() == '\\')
          name += "\\";

        if(value.back() == '\\')
          value += "\\";

        cmdWithEnv += L"\"" + std::wstring(StringFormat::UTF82Wide(name).c_str()) + L"\" ";
        cmdWithEnv += L"\"" + std::wstring(StringFormat::UTF82Wide(value).c_str()) + L"\" ";
      }

      commandLine = (wchar_t *)cmdWithEnv.c_str();
    }

    BOOL retValue = CreateProcessW(NULL, commandLine, &pSec, &tSec, false,
                                   CREATE_NEW_CONSOLE | CREATE_SUSPENDED, NULL, NULL, &si, &pi);

    SAFE_DELETE_ARRAY(paramsAlloc);

    if(!retValue)
    {
      RDResult result;
#if RENDERDOC_OFFICIAL_BUILD
      SET_ERROR_RESULT(result, ResultCode::InternalError,
                       "Can't run 32-bit renderdoccmd to capture 32-bit program.");
#else
      SET_ERROR_RESULT(
          result, ResultCode::InternalError,
          "Can't run 32-bit renderdoccmd to capture 32-bit program."
          "If this is a locally built tool you must build both 32-bit and 64-bit versions.");
#endif
      CloseHandle(hProcess);
      return {result, 0};
    }

    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hThread, INFINITE);
    CloseHandle(pi.hThread);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);

    if(waitForExit)
      WaitForSingleObject(hProcess, INFINITE);

    CloseHandle(hProcess);

    if(exitCode == 0)
    {
      RDResult result;
      SET_ERROR_RESULT(result, ResultCode::UnknownError,
                       "Encountered error while launching target 32-bit program.");
      return {result, 0};
    }

    if(exitCode < RenderDoc_FirstTargetControlPort)
    {
      ResultCode code = (ResultCode)exitCode;

      RDResult result;
      SET_ERROR_RESULT(result, code, "32-bit renderdoccmd returned '%s'", ToStr(code).c_str());
      return {code, 0};
    }

    return {ResultCode::Succeeded, (uint32_t)exitCode};
  }

  InjectDLL(hProcess, renderdocPath);

  const char *rdoc_dll = STRINGIZE(RDOC_BASE_NAME);

  uintptr_t loc = FindRemoteDLL(pid, STRINGIZE(RDOC_BASE_NAME) ".dll");

  CloseHandle(hProcess);
  hProcess = NULL;

  rdcpair<RDResult, uint32_t> result = {ResultCode::Succeeded, 0};

  if(loc == 0)
  {
    SET_ERROR_RESULT(
        result.first, ResultCode::InjectionFailed,
        "Failed to inject %s.dll into process. Check that the process did not crash or exit "
        "early in initialisation, e.g. if the working directory is incorrectly set.",
        rdoc_dll);
  }
  else
  {
    hProcess =
        OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                        PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE,
                    FALSE, pid);

    if(!hProcess)
    {
      SET_ERROR_RESULT(result.first, ResultCode::InjectionFailed,
                       "Couldn't reopen process %u after injection (err %u).", pid, GetLastError());
    }
    else
    {
      // safe to cast away the const as we know these functions don't modify the parameters

      if(!capturefile.empty())
        InjectFunctionCall(hProcess, loc, "INTERNAL_SetCaptureFile", (void *)capturefile.c_str(),
                           capturefile.size() + 1);

      rdcstr debugLogfile = RDCGETLOGFILE();

      InjectFunctionCall(hProcess, loc, "INTERNAL_SetDebugLogFile", (void *)debugLogfile.c_str(),
                         debugLogfile.size() + 1);

      InjectFunctionCall(hProcess, loc, "INTERNAL_SetCaptureOptions", (CaptureOptions *)&opts,
                         sizeof(CaptureOptions));

      InjectFunctionCall(hProcess, loc, "INTERNAL_GetTargetControlIdent", &result.second,
                         sizeof(result.second));

      if(!env.empty())
      {
        for(const EnvironmentModification &e : env)
        {
          rdcstr name = e.name.trimmed();
          rdcstr value = e.value;
          EnvMod mod = e.mod;
          EnvSep sep = e.sep;

          if(name == "")
            break;

          InjectFunctionCall(hProcess, loc, "INTERNAL_EnvModName", (void *)name.c_str(),
                             name.size() + 1);
          InjectFunctionCall(hProcess, loc, "INTERNAL_EnvModValue", (void *)value.c_str(),
                             value.size() + 1);
          InjectFunctionCall(hProcess, loc, "INTERNAL_EnvSep", &sep, sizeof(sep));
          InjectFunctionCall(hProcess, loc, "INTERNAL_EnvMod", &mod, sizeof(mod));
        }

        // parameter is unused
        void *dummy = NULL;
        InjectFunctionCall(hProcess, loc, "INTERNAL_ApplyEnvMods", &dummy, sizeof(dummy));
      }
    }
  }

  if(waitForExit && hProcess)
    WaitForSingleObject(hProcess, INFINITE);

  if(hProcess)
    CloseHandle(hProcess);

  // 清理主线程 ID 记录，注入流程已完成
  s_MainThreadIds.erase(pid);

  return result;
}

uint32_t Process::LaunchProcess(const rdcstr &app, const rdcstr &workingDir, const rdcstr &cmdLine,
                                bool internal, ProcessResult *result)
{
  HANDLE hChildStdOutput_Rd = NULL, hChildStdError_Rd = NULL;

  rdcstr appPath = app;
  size_t len = appPath.length();
  rdcstr ext;
  if(len > 4)
    ext = strlower(appPath.substr(len - 4));
  if(ext != ".exe")
    appPath += ".exe";

  PROCESS_INFORMATION pi =
      RunProcess(appPath, workingDir, cmdLine, {}, internal, result ? &hChildStdOutput_Rd : NULL,
                 result ? &hChildStdError_Rd : NULL);

  if(pi.dwProcessId == 0)
  {
    if(!internal)
      RDCWARN("Couldn't launch process '%s'", appPath.c_str());

    if(hChildStdError_Rd != NULL)
      CloseHandle(hChildStdError_Rd);
    if(hChildStdOutput_Rd != NULL)
      CloseHandle(hChildStdOutput_Rd);

    return 0;
  }

  if(!internal)
    RDCLOG("Launched process '%s' with '%s'", appPath.c_str(), cmdLine.c_str());

  ResumeThread(pi.hThread);

  if(result)
  {
    result->strStdout = "";
    result->strStderror = "";

    char chBuf[4096];
    DWORD dwOutputRead, dwErrorRead;
    BOOL success = FALSE;
    rdcstr s;
    for(;;)
    {
      success = ReadFile(hChildStdOutput_Rd, chBuf, sizeof(chBuf), &dwOutputRead, NULL);
      s = rdcstr(chBuf, dwOutputRead);
      result->strStdout += s;

      if(!success && !dwOutputRead)
        break;
    }

    for(;;)
    {
      success = ReadFile(hChildStdError_Rd, chBuf, sizeof(chBuf), &dwErrorRead, NULL);
      s = rdcstr(chBuf, dwErrorRead);
      result->strStderror += s;

      if(!success && !dwErrorRead)
        break;
    }

    CloseHandle(hChildStdOutput_Rd);
    CloseHandle(hChildStdError_Rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, (LPDWORD)&result->retCode);
  }

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  return pi.dwProcessId;
}

uint32_t Process::LaunchScript(const rdcstr &script, const rdcstr &workingDir,
                               const rdcstr &argList, bool internal, ProcessResult *result)
{
  // Change parameters to invoke command interpreter
  rdcstr args = "/C " + script + " " + argList;

  return LaunchProcess("cmd.exe", workingDir, args, internal, result);
}

rdcpair<RDResult, uint32_t> Process::LaunchAndInjectIntoProcess(
    const rdcstr &app, const rdcstr &workingDir, const rdcstr &cmdLine,
    const rdcarray<EnvironmentModification> &env, const rdcstr &capturefile,
    const CaptureOptions &opts, bool waitForExit)
{
  void *func =
      GetProcAddress(GetModuleHandleA(STRINGIZE(RDOC_BASE_NAME) ".dll"), "INTERNAL_SetCaptureFile");

  if(func == NULL)
  {
    const char *rdoc_dll = STRINGIZE(RDOC_BASE_NAME);
    RDResult result;
    SET_ERROR_RESULT(result, ResultCode::InternalError,
                     "Can't find required export function in %s.dll - corrupted/missing file?",
                     rdoc_dll);
    return {result, 0};
  }

  if(get_basename(app) == "explorer.exe" || get_basename(app) == "dllhost.exe")
  {
    RDResult result;
    SET_ERROR_RESULT(
        result, ResultCode::InjectionFailed,
        "For safety reasons this tool does not support capturing executables with a "
        "reserved system filename such as '%s'. Please rename your executable to capture.",
        get_basename(app).c_str());
    return {result, 0};
  }

  PROCESS_INFORMATION pi = RunProcess(app, workingDir, cmdLine, env, false, NULL, NULL);

  if(pi.dwProcessId == 0)
  {
    RDResult result;
    SET_ERROR_RESULT(result, ResultCode::InjectionFailed, "Failed to launch process.");
    return {result, 0};
  }

  rdcpair<RDResult, uint32_t> ret = InjectIntoProcess(pi.dwProcessId, {}, capturefile, opts, false);

  CloseHandle(pi.hProcess);
  // 恢复线程：InjectIntoProcess 完成后，线程处于挂起状态（挂起计数 = 1）。
  // 这个挂起来自最后一次 InjectFunctionCall 中 shellcode 的 SuspendThread(GetCurrentThread())。
  // 需要一次 ResumeThread 使线程运行。使用循环确保安全。
  DWORD suspCount;
  do {
    suspCount = ResumeThread(pi.hThread);
  } while(suspCount > 1);

  if(ret.second == 0 || ret.first != ResultCode::Succeeded)
  {
    CloseHandle(pi.hThread);
    return ret;
  }

  if(waitForExit)
    WaitForSingleObject(pi.hThread, INFINITE);

  CloseHandle(pi.hThread);

  return ret;
}

bool Process::CanGlobalHook()
{
  // all we need is admin rights and it's the caller's responsibility to ensure that.
  return true;
}

// to simplify the below code, rather than splitting by 32-bit/64-bit we split by native and Wow32.
// This means that for 32-bit code (whether it's on 32-bit OS or not) we just have native, and the
// Wow32 stuff is empty/unused. For 64-bit we use both. Thus the native registry key is always the
// same path regardless of the bitness we're running as and we don't have to move things around or
// have conditionals all over

struct GlobalHookData
{
  struct
  {
    HANDLE pipe = NULL;
    DWORD appinitEnabled = 0;
    rdcwstr appinitDLLs;
  } dataNative, dataWow32;

  int32_t finished = 0;
  Threading::ThreadHandle pipeThread = 0;
};

// utility function to close the registry keys, print an error, and quit
static RDResult HandleRegError(HKEY keyNative, HKEY keyWow32, LSTATUS ret, const char *msg)
{
  if(keyNative)
    RegCloseKey(keyNative);

  if(keyWow32)
    RegCloseKey(keyWow32);

  RDCLOG("Error with AppInit registry keys - %s (%d)", msg, ret);

  RETURN_ERROR_RESULT(ResultCode::InjectionFailed,
                      "Error updating registry to enable global hook.\n"
                      "Check that the tool is correctly running as administrator.");
}

#define REG_CHECK(msg)                                    \
  if(ret != ERROR_SUCCESS)                                \
  {                                                       \
    return HandleRegError(keyNative, keyWow32, ret, msg); \
  }

// function to backup the previous settings for AppInit, then enable it and write our own paths.
RDResult BackupAndChangeRegistry(GlobalHookData &hookdata, const rdcstr &shimpathWow32,
                                 const rdcstr &shimpathNative)
{
  HKEY keyNative = NULL;
  HKEY keyWow32 = NULL;

  // AppInit_DLLs requires short paths, but short paths can be disabled globally or on a per-volume
  // level. If short paths are disabled we'll get the long path back, we *always* expect the path to
  // get shorter because the shim filename is bigger than 8.3.

  DWORD nativeShortSize = GetShortPathNameW(StringFormat::UTF82Wide(shimpathNative).c_str(), NULL,
                                            (DWORD)shimpathNative.length());
  if(nativeShortSize == (DWORD)shimpathNative.length() + 1)
  {
    RETURN_ERROR_RESULT(
        ResultCode::FileIOFailed,
        "The tool is installed on a volume or system that has short paths disabled.\n"
        "For the global hook, short paths must be enabled where the tool is installed.");
  }

  if(!shimpathWow32.empty())
  {
    DWORD wow32ShortSize = GetShortPathNameW(StringFormat::UTF82Wide(shimpathWow32).c_str(), NULL,
                                             (DWORD)shimpathWow32.length());

    if(wow32ShortSize == (DWORD)shimpathWow32.length() + 1)
    {
      RETURN_ERROR_RESULT(
          ResultCode::FileIOFailed,
          "The tool is installed on a volume or system that has short paths disabled.\n"
          "For the global hook, short paths must be enabled where the tool is installed.");
    }
  }

  // open the native key
  LSTATUS ret = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows", 0, NULL,
                                0, KEY_READ | KEY_WRITE, NULL, &keyNative, NULL);

  REG_CHECK("Could not open AppInit key");

  // if we are doing Wow32, open that key as well
  if(!shimpathWow32.empty())
  {
    ret = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                          "SOFTWARE\\Wow6432Node\\Microsoft\\Windows NT\\CurrentVersion\\Windows",
                          0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &keyWow32, NULL);

    REG_CHECK("Could not open AppInit key");
  }

  const DWORD one = 1;

  // fetch the previous data for LoadAppInit_DLLs and AppInit_DLLs
  DWORD sz = 4;
  ret = RegGetValueA(keyNative, NULL, "LoadAppInit_DLLs", RRF_RT_REG_DWORD, NULL,
                     (void *)&hookdata.dataNative.appinitEnabled, &sz);
  REG_CHECK("Could not fetch LoadAppInit_DLLs");

  sz = 0;
  ret = RegGetValueW(keyNative, NULL, L"AppInit_DLLs", RRF_RT_ANY, NULL, NULL, &sz);
  if(ret == ERROR_MORE_DATA || ret == ERROR_SUCCESS)
  {
    hookdata.dataNative.appinitDLLs = rdcwstr(sz / sizeof(wchar_t));
    ret = RegGetValueW(keyNative, NULL, L"AppInit_DLLs", RRF_RT_ANY, NULL,
                       hookdata.dataNative.appinitDLLs.data(), &sz);
  }
  REG_CHECK("Could not fetch AppInit_DLLs");

  // set DWORD:1 for LoadAppInit_DLLs and convert our path to a short path then set it
  ret = RegSetValueExA(keyNative, "LoadAppInit_DLLs", 0, REG_DWORD, (const BYTE *)&one, sizeof(one));
  REG_CHECK("Could not set LoadAppInit_DLLs");

  rdcwstr shortpath(shimpathNative.size());
  GetShortPathNameW(StringFormat::UTF82Wide(shimpathNative).c_str(), shortpath.data(),
                    (DWORD)shortpath.length());

  ret = RegSetValueExW(keyNative, L"AppInit_DLLs", 0, REG_SZ, (const BYTE *)shortpath.data(),
                       DWORD(shortpath.length() * sizeof(wchar_t)));
  REG_CHECK("Could not set AppInit_DLLs");

  // if we're doing Wow32, repeat the process for those keys
  if(keyWow32)
  {
    sz = 4;
    ret = RegGetValueA(keyWow32, NULL, "LoadAppInit_DLLs", RRF_RT_REG_DWORD, NULL,
                       (void *)&hookdata.dataWow32.appinitEnabled, &sz);
    REG_CHECK("Could not fetch LoadAppInit_DLLs");

    sz = 0;
    ret = RegGetValueW(keyWow32, NULL, L"AppInit_DLLs", RRF_RT_ANY, NULL, NULL, &sz);
    if(ret == ERROR_MORE_DATA || ret == ERROR_SUCCESS)
    {
      hookdata.dataWow32.appinitDLLs = rdcwstr(sz / sizeof(wchar_t));
      ret = RegGetValueW(keyWow32, NULL, L"AppInit_DLLs", RRF_RT_ANY, NULL,
                         hookdata.dataWow32.appinitDLLs.data(), &sz);
    }
    REG_CHECK("Could not fetch AppInit_DLLs");

    ret = RegSetValueExA(keyWow32, "LoadAppInit_DLLs", 0, REG_DWORD, (const BYTE *)&one, sizeof(one));
    REG_CHECK("Could not set LoadAppInit_DLLs");

    shortpath = rdcwstr(shimpathWow32.size());
    GetShortPathNameW(StringFormat::UTF82Wide(shimpathWow32).c_str(), shortpath.data(),
                      (DWORD)shortpath.length());

    ret = RegSetValueExW(keyWow32, L"AppInit_DLLs", 0, REG_SZ, (const BYTE *)shortpath.data(),
                         DWORD(shortpath.length() * sizeof(wchar_t)));
    REG_CHECK("Could not set AppInit_DLLs");
  }

  std::wstring backup;

  // write a .reg file that contains the previous settings, so that if all else fails the user can
  // manually insert it back into the registry to restore everything.
  backup += L"Windows Registry Editor Version 5.00\n";
  backup += L"\n";
  backup += L"[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows]\n";
  backup += L"\"LoadAppInit_DLLs\"=dword:0000000";
  backup += (hookdata.dataNative.appinitEnabled ? L"1\n" : L"0\n");
  backup += L"\"AppInit_DLLs\"=\"";
  // we append with the C string so we don't add trailing NULLs into the text.
  backup += hookdata.dataNative.appinitDLLs.c_str();
  backup += L"\"\n";
  if(keyWow32)
  {
    backup += L"\n";
    backup +=
        L"[HKEY_LOCAL_MACHINE\\SOFTWARE\\Wow6432Node\\Microsoft\\"
        L"Windows NT\\CurrentVersion\\Windows]\n";
    backup += L"\"LoadAppInit_DLLs\"=dword:0000000";
    backup += (hookdata.dataWow32.appinitEnabled ? L"1\n" : L"0\n");
    backup += L"\"AppInit_DLLs\"=\"";
    backup += hookdata.dataWow32.appinitDLLs.c_str();
    backup += L"\"\n";
  }

  if(keyNative)
    RegCloseKey(keyNative);

  if(keyWow32)
    RegCloseKey(keyWow32);

  keyNative = keyWow32 = NULL;

  // write it to disk but don't fail if we can't, just print it to the log and keep going.
  wchar_t reg_backup[MAX_PATH];
  GetTempPathW(MAX_PATH, reg_backup);
  wcscat_s(reg_backup, L"GfxDiag_RestoreGlobalHook.reg");

  FILE *f = NULL;
  _wfopen_s(&f, reg_backup, L"w");
  if(f)
  {
    fputws(backup.c_str(), f);
    fclose(f);
  }
  else
  {
    RDCERR("Error opening registry backup file %ls", reg_backup);
    RDCERR("Backup registry data is:\n\n%ls\n\n", backup.c_str());
  }

  return RDResult();
}

// switch error-handling to print-and-continue, as we can't really do anything about it at this
// point and we want to continue restoring in case only one thing failed.
#undef REG_CHECK
#define REG_CHECK(msg)                                                      \
  if(ret != ERROR_SUCCESS)                                                  \
  {                                                                         \
    HandleRegError(keyNative, keyWow32, ret, "Could not open AppInit key"); \
  }

void RestoreRegistry(const GlobalHookData &hookdata)
{
  HKEY keyNative = NULL;
  HKEY keyWow32 = NULL;
  LSTATUS ret = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows", 0, NULL,
                                0, KEY_READ | KEY_WRITE, NULL, &keyNative, NULL);

  REG_CHECK("Could not open AppInit key");

#if ENABLED(RDOC_X64)
  ret = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                        "SOFTWARE\\Wow6432Node\\Microsoft\\Windows NT\\CurrentVersion\\Windows", 0,
                        NULL, 0, KEY_READ | KEY_WRITE, NULL, &keyWow32, NULL);

  REG_CHECK("Could not open AppInit key");
#endif

  // set the native values back to where they were
  ret = RegSetValueExA(keyNative, "LoadAppInit_DLLs", 0, REG_DWORD,
                       (const BYTE *)&hookdata.dataNative.appinitEnabled,
                       sizeof(hookdata.dataNative.appinitEnabled));
  REG_CHECK("Could not set LoadAppInit_DLLs");

  ret = RegSetValueExW(keyNative, L"AppInit_DLLs", 0, REG_SZ,
                       (const BYTE *)hookdata.dataNative.appinitDLLs.c_str(),
                       DWORD(hookdata.dataNative.appinitDLLs.length() * sizeof(wchar_t)));
  REG_CHECK("Could not set AppInit_DLLs");

  // if we opened it, restore the Wow32 values as well
  if(keyWow32)
  {
    ret = RegSetValueExA(keyWow32, "LoadAppInit_DLLs", 0, REG_DWORD,
                         (const BYTE *)&hookdata.dataWow32.appinitEnabled,
                         sizeof(hookdata.dataWow32.appinitEnabled));
    REG_CHECK("Could not set LoadAppInit_DLLs");

    ret = RegSetValueExW(keyWow32, L"AppInit_DLLs", 0, REG_SZ,
                         (const BYTE *)hookdata.dataWow32.appinitDLLs.c_str(),
                         DWORD(hookdata.dataWow32.appinitDLLs.length() * sizeof(wchar_t)));
    REG_CHECK("Could not set AppInit_DLLs");
  }
}

static GlobalHookData *globalHook = NULL;

// a thread we run in the background just to keep the pipes open and wait until we're ready to stop
// the global hook.
static void GlobalHookThread()
{
  Threading::SetCurrentThreadName("GlobalHookThread");

  // keep looping doing an atomic compare-exchange to check that finished is still 0
  while(Atomic::CmpExch32(&globalHook->finished, 0, 0) == 0)
  {
    // wake every quarter of a second to test again
    Threading::Sleep(250);
  }

  char exitData[32] = "exit";

  // write some data into the pipe and close it. The data is (currently) unimportant, just that it
  // causes the blocking read on the other end to succeed and close the program.
  DWORD dummy = 0;
  if(globalHook->dataNative.pipe)
  {
    WriteFile(globalHook->dataNative.pipe, exitData, (DWORD)sizeof(exitData), &dummy, NULL);
    CloseHandle(globalHook->dataNative.pipe);
  }

  if(globalHook->dataWow32.pipe)
  {
    WriteFile(globalHook->dataWow32.pipe, exitData, (DWORD)sizeof(exitData), &dummy, NULL);
    CloseHandle(globalHook->dataWow32.pipe);
  }
}

RDResult Process::StartGlobalHook(const rdcstr &pathmatch, const rdcstr &capturefile,
                                  const CaptureOptions &opts)
{
  if(pathmatch.empty())
  {
    RETURN_ERROR_RESULT(ResultCode::InvalidParameter,
                        "Invalid global hook parameter, empty path to match");
  }

  rdcstr renderdocPath;
  FileIO::GetLibraryFilename(renderdocPath);

  renderdocPath = get_dirname(renderdocPath);

  // the native renderdoccmd.exe is always next to the dll. Wow32 will be somewhere else
  rdcstr cmdpathNative = renderdocPath + "\\gfxdiagcmd.exe";
  rdcstr cmdpathWow32;

  rdcstr shimpathNative = renderdocPath;
  rdcstr shimpathWow32;

#if ENABLED(RDOC_X64)

  // native shim is just renderdocshim64.dll
  shimpathNative = renderdocPath + "\\gfxdiagshim64.dll";

  // if it looks like we're in the development environment, look for the alternate bitness in the
  // corresponding folder
  int devLocation = renderdocPath.find("\\x64\\Development");
  if(devLocation >= 0)
  {
    renderdocPath.erase(devLocation, ~0U);

    shimpathWow32 = renderdocPath + "\\Win32\\Development\\gfxdiagshim32.dll";
    cmdpathWow32 = renderdocPath + "\\Win32\\Development\\gfxdiagcmd.exe";
  }
  else
  {
    devLocation = renderdocPath.find("\\x64\\Release");

    if(devLocation >= 0)
    {
      renderdocPath.erase(devLocation, ~0U);

      shimpathWow32 = renderdocPath + "\\Win32\\Release\\gfxdiagshim32.dll";
      cmdpathWow32 = renderdocPath + "\\Win32\\Release\\gfxdiagcmd.exe";
    }
  }

  // if we're not in the dev environment, assume it's under a x86\ subfolder
  if(devLocation < 0)
  {
    shimpathWow32 = renderdocPath + "\\x86\\gfxdiagshim32.dll";
    cmdpathWow32 = renderdocPath + "\\x86\\gfxdiagcmd.exe";
  }

#else

  // nothing fancy to do here for 32-bit, just point the shim next to our dll.
  shimpathNative = renderdocPath + "\\gfxdiagshim32.dll";

#endif

  GlobalHookData hookdata;

  // try to backup and change the registry settings to start loading our shim dlls. If that fails,
  // we bail out immediately
  RDResult regStatus = BackupAndChangeRegistry(hookdata, shimpathWow32, shimpathNative);
  if(regStatus != ResultCode::Succeeded)
    return regStatus;

  PROCESS_INFORMATION pi = {0};
  STARTUPINFO si = {0};
  SECURITY_ATTRIBUTES pSec = {0};
  SECURITY_ATTRIBUTES tSec = {0};
  pSec.nLength = sizeof(pSec);
  tSec.nLength = sizeof(tSec);

  si.cb = sizeof(si);

  // serialise to string with two chars per byte
  rdcstr optstr = opts.EncodeAsString();
  rdcstr debugLogfile = RDCGETLOGFILE();

  rdcstr params = StringFormat::Fmt(
      "\"%s\" globalhook --match \"%s\" --capfile \"%s\" --debuglog \"%s\" --capopts \"%s\"",
      cmdpathNative.c_str(), pathmatch.c_str(), capturefile.c_str(), debugLogfile.c_str(),
      optstr.c_str());

  rdcwstr paramsAlloc = StringFormat::UTF82Wide(params);

  // we'll be setting stdin
  si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;

  // hide the console window
  si.wShowWindow = SW_HIDE;

  // this is the end of the pipe that the child will inherit and use as stdin
  HANDLE childEnd = NULL;

  DWORD err;

  // create a pipe with the writing end for us, and the reading end as the child process's stdin
  {
    SECURITY_ATTRIBUTES pipeSec;
    pipeSec.nLength = sizeof(SECURITY_ATTRIBUTES);
    pipeSec.bInheritHandle = TRUE;
    pipeSec.lpSecurityDescriptor = NULL;

    BOOL res;
    res = CreatePipe(&childEnd, &hookdata.dataNative.pipe, &pipeSec, 0);

    if(!res)
    {
      err = GetLastError();
      RestoreRegistry(hookdata);
      RETURN_ERROR_RESULT(ResultCode::InternalError, "Could not create 32-bit stdin pipe (err %u)",
                          err);
    }

    // we don't want the child process to inherit our end
    res = SetHandleInformation(hookdata.dataNative.pipe, HANDLE_FLAG_INHERIT, 0);

    if(!res)
    {
      err = GetLastError();
      RestoreRegistry(hookdata);
      RETURN_ERROR_RESULT(ResultCode::InternalError,
                          "Could not make 32-bit stdin pipe inheritable (err %u)", err);
    }

    si.hStdInput = childEnd;
  }

  // launch the process
  BOOL retValue = CreateProcessW(NULL, &paramsAlloc[0], &pSec, &tSec, true, CREATE_NEW_CONSOLE,
                                 NULL, NULL, &si, &pi);

  err = GetLastError();

  // we don't need this end anymore, the child has it
  CloseHandle(childEnd);

  if(retValue == FALSE)
  {
    CloseHandle(hookdata.dataNative.pipe);
    RestoreRegistry(hookdata);
    RETURN_ERROR_RESULT(ResultCode::InternalError, "Can't launch renderdoccmd from '%s' (err %u)",
                        cmdpathNative.c_str(), err);
  }

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  RDCEraseEl(pi);

// repeat the process for the Wow32 renderdoccmd
#if ENABLED(RDOC_X64)
  params = StringFormat::Fmt(
      "\"%s\" globalhook --match \"%s\" --capfile \"%s\" --debuglog \"%s\" --capopts \"%s\"",
      cmdpathWow32.c_str(), pathmatch.c_str(), capturefile.c_str(), debugLogfile.c_str(),
      optstr.c_str());

  paramsAlloc = StringFormat::UTF82Wide(params);

  {
    SECURITY_ATTRIBUTES pipeSec;
    pipeSec.nLength = sizeof(SECURITY_ATTRIBUTES);
    pipeSec.bInheritHandle = TRUE;
    pipeSec.lpSecurityDescriptor = NULL;

    BOOL res;
    res = CreatePipe(&childEnd, &hookdata.dataWow32.pipe, &pipeSec, 0);

    if(!res)
    {
      err = GetLastError();
      RestoreRegistry(hookdata);
      RETURN_ERROR_RESULT(ResultCode::InternalError, "Could not create 64-bit stdin pipe (err %u)",
                          err);
    }

    res = SetHandleInformation(hookdata.dataWow32.pipe, HANDLE_FLAG_INHERIT, 0);

    if(!res)
    {
      err = GetLastError();
      RestoreRegistry(hookdata);
      RETURN_ERROR_RESULT(ResultCode::InternalError,
                          "Could not make 64-bit stdin pipe inheritable (err %u)", err);
    }

    si.hStdInput = childEnd;
  }

  retValue = CreateProcessW(NULL, &paramsAlloc[0], &pSec, &tSec, true, CREATE_NEW_CONSOLE, NULL,
                            NULL, &si, &pi);

  err = GetLastError();

  // we don't need this end anymore
  CloseHandle(childEnd);

  if(retValue == FALSE)
  {
    CloseHandle(hookdata.dataNative.pipe);
    CloseHandle(hookdata.dataWow32.pipe);
    RestoreRegistry(hookdata);
    RETURN_ERROR_RESULT(ResultCode::InternalError, "Can't launch renderdoccmd from '%s' (err %u)",
                        cmdpathWow32.c_str(), err);
  }

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
#endif

  // set static global pointer with our data, and launch the thread
  globalHook = new GlobalHookData;
  *globalHook = hookdata;

  globalHook->pipeThread = Threading::CreateThread(&GlobalHookThread);

  return RDResult();
}

bool Process::IsGlobalHookActive()
{
  return globalHook != NULL;
}
void Process::StopGlobalHook()
{
  if(!globalHook)
    return;

  // set the finished flag and join to the thread so it closes the pipes (and so the child
  // processes)
  Atomic::Inc32(&globalHook->finished);

  Threading::JoinThread(globalHook->pipeThread);
  Threading::CloseThread(globalHook->pipeThread);

  // restore the registry settings from before we started
  RestoreRegistry(*globalHook);

  delete globalHook;
  globalHook = NULL;
}

bool Process::IsModuleLoaded(const rdcstr &module)
{
  return GetModuleHandleA(module.c_str()) != NULL;
}

void *Process::LoadModule(const rdcstr &module)
{
  HMODULE mod = GetModuleHandleA(module.c_str());
  if(mod != NULL)
    return mod;

  return LoadLibraryA(module.c_str());
}

void *Process::GetFunctionAddress(void *module, const rdcstr &function)
{
  if(module == NULL)
    return NULL;

  return (void *)GetProcAddress((HMODULE)module, function.c_str());
}

uint32_t Process::GetCurrentPID()
{
  return (uint32_t)GetCurrentProcessId();
}

void Process::Shutdown()
{
  // nothing to do
}
