/*********************************************************
 * Copyright (C) 2010 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the Lesser GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *********************************************************/


/*
 * procMgrSolaris.c --
 *
 *    Solaris specific implementations of the process management lib methods.
 *
 */


#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <procfs.h>
#include <ctype.h>
#include <sys/param.h>

#include "vmware.h"
#include "procMgr.h"
#include "debug.h"
#include "util.h"
#undef offsetof
#include "dynbuf.h"
#include "dynarray.h"
#include "su.h"
#include "str.h"
#include "fileIO.h"

static Bool
ReadArgsFromAddressSpaceFile(FileIODescriptor asFd,
                             psinfo_t *psInfo,
                             DynBufArray *cmdLineArr);

static Bool
ReadOffsetsFromAddressSpaceFile(FileIODescriptor asFd,
                                psinfo_t *psInfo,
                                uintptr_t *argOffs);

static char *
ExtractArgStringFromAddressSpaceFile(FileIODescriptor asFd,
                                     uintptr_t offset);

static char *
ExtractCommandLineFromAddressSpaceFile(psinfo_t *procInfo);

/*
 *----------------------------------------------------------------------------
 *
 * ProcMgr_ListProcesses --
 *
 *      List all the processes that the calling client has privilege to
 *      enumerate. The strings in the returned structure should be all
 *      UTF-8 encoded, although we do not enforce it right now.
 *
 * Results:
 *
 *      On success, the process list is returned and the caller is responsible
 *      for freeing the memory used by calling ProcMgr_FreeProcList. On
 *      failure, NULL is returned.
 *
 * Side effects:
 *
 *      None
 *
 *----------------------------------------------------------------------------
 */

ProcMgr_ProcList *
ProcMgr_ListProcesses(void)
{
   ProcMgr_ProcList *procList = NULL;

   Bool failed = FALSE;
   DynBuf dbProcId;
   DynBuf dbProcCmd;
   DynBuf dbProcStartTime;
   DynBuf dbProcOwner;
   DIR *dir;
   struct dirent *ent;

   DynBuf_Init(&dbProcId);
   DynBuf_Init(&dbProcCmd);
   DynBuf_Init(&dbProcStartTime);
   DynBuf_Init(&dbProcOwner);

   dir = opendir("/proc");
   if (NULL == dir) {
      Warning("ProcMgr_ListProcesses unable to open /proc\n");
      failed = TRUE;
      goto exit;
   }

   while (TRUE) {
      pid_t pid;
      struct passwd *pwd;
      char *cmdLineTemp;
      char *userName = NULL;
      char tempPath[MAXPATHLEN];
      time_t processStartTime;
      psinfo_t procInfo;
      size_t strLen = 0;
      size_t numRead = 0;
      FileIODescriptor psInfoFd;
      FileIOResult res;

      errno = 0;
      ent = readdir(dir);
      if (ent == NULL) {
         if (errno == 0) {
            break;
         } else {
            failed = TRUE;
            goto exit;
         }
      }

      if (Str_Snprintf(tempPath,
                       sizeof tempPath,
                       "/proc/%s/psinfo",
                       ent->d_name) == -1) {
         Debug("Process id '%s' too large\n", ent->d_name);
         continue;
      }
      res = FileIO_Open(&psInfoFd,
                        tempPath,
                        FILEIO_OPEN_ACCESS_READ,
                        FILEIO_OPEN);
      if (res != FILEIO_SUCCESS) {
         if ((res == FILEIO_FILE_NOT_FOUND) ||
             (res == FILEIO_NO_PERMISSION)) {
            continue;
         } else {
            failed = TRUE;
            goto exit;
         }
      }

      res = FileIO_Read(&psInfoFd, &procInfo, sizeof procInfo, &numRead);
      FileIO_Close(&psInfoFd);
      if (res != FILEIO_SUCCESS) {
         failed = TRUE;
         goto exit;
      }

      processStartTime = procInfo.pr_start.tv_sec;

      /*
       * Command line strings in procInfo.pr_psargs are truncated to PRARGZ
       * bytes. In this case we extract the arguments from the /proc/<pid>/as
       * file. Since most command line strings are expected to fit within
       * PRARGSZ bytes, we avoid calling
       * ExtractCommandLineFromAddressSpaceFile for every process.
       */
      if (strlen(procInfo.pr_psargs) + 1 == PRARGSZ) {
         cmdLineTemp = ExtractCommandLineFromAddressSpaceFile(&procInfo);
         if(cmdLineTemp == NULL) {
            cmdLineTemp = Util_SafeStrdup(procInfo.pr_psargs);
         }
      } else {
         cmdLineTemp = Util_SafeStrdup(procInfo.pr_psargs);
      }

      /*
       * Store the command line string pointer in dynbuf.
       */
      DynBuf_Append(&dbProcCmd, &cmdLineTemp, sizeof cmdLineTemp);

      /*
       * Store the pid in dynbuf.
       */
      pid = procInfo.pr_pid;
      DynBuf_Append(&dbProcId, &pid, sizeof pid);

      /*
       * Store the owner of the process.
       */
      pwd = getpwuid(procInfo.pr_uid);
      userName = (NULL == pwd)
                 ? Str_Asprintf(&strLen, "%d", (int) procInfo.pr_uid)
                 : Util_SafeStrdup(pwd->pw_name);
      DynBuf_Append(&dbProcOwner, &userName, sizeof userName);

      /*
       * Store the time that the process started.
       */
      DynBuf_Append(&dbProcStartTime,
                    &processStartTime,
                    sizeof processStartTime);
   } // while (TRUE)

   closedir(dir);

   if (0 == DynBuf_GetSize(&dbProcId)) {
      failed = TRUE;
      goto exit;
   }

   /*
    * We're done adding to DynBuf.  Trim off any unused allocated space.
    * DynBuf_Trim() followed by DynBuf_Detach() avoids a memcpy().
    */
   DynBuf_Trim(&dbProcId);
   DynBuf_Trim(&dbProcCmd);
   DynBuf_Trim(&dbProcStartTime);
   DynBuf_Trim(&dbProcOwner);

   /*
    * Create a ProcMgr_ProcList and populate its fields.
    */
   procList = (ProcMgr_ProcList *) Util_SafeCalloc(1, sizeof(ProcMgr_ProcList));
   ASSERT_MEM_ALLOC(procList);

   procList->procCount = DynBuf_GetSize(&dbProcId) / sizeof(pid_t);

   procList->procIdList = (pid_t *) DynBuf_Detach(&dbProcId);
   ASSERT_MEM_ALLOC(procList->procIdList);
   procList->procCmdList = (char **) DynBuf_Detach(&dbProcCmd);
   ASSERT_MEM_ALLOC(procList->procCmdList);
   procList->startTime = (time_t *) DynBuf_Detach(&dbProcStartTime);
   ASSERT_MEM_ALLOC(procList->startTime);
   procList->procOwnerList = (char **) DynBuf_Detach(&dbProcOwner);
   ASSERT_MEM_ALLOC(procList->procOwnerList);

exit:
   DynBuf_Destroy(&dbProcId);
   DynBuf_Destroy(&dbProcCmd);
   DynBuf_Destroy(&dbProcStartTime);
   DynBuf_Destroy(&dbProcOwner);

   if (failed) {
      ProcMgr_FreeProcList(procList);
      procList = NULL;
   }

   return procList;
}


/*
 *----------------------------------------------------------------------------
 *
 * ExtractCommandLineFromAddressSpaceFile --
 *
 *      Read the address space file (/proc/<pid>/as) for a given process and
 *      return its command line string.
 *
 * Results:
 *
 *      On success, the command line string for the process is returned and the
 *      caller is responsible for freeing the memory used by this string. On
 *      failure, NULL is returned.
 *
 * Side effects:
 *
 *      None
 *
 *----------------------------------------------------------------------------
 */

static char *
ExtractCommandLineFromAddressSpaceFile(psinfo_t *procInfo) //IN: psinfo struct
{
   int argc;
   int i;
   char tempPath[MAXPATHLEN];
   char *buf;
   FileIODescriptor asFd;
   FileIOResult res;
   DynBuf cmdLine;
   DynBufArray args;
   pid_t pid;

   pid = procInfo->pr_pid;
   if (Str_Snprintf(tempPath,
                sizeof tempPath,
                "/proc/%"FMT64"d/as",
                (int64_t)pid) == -1) {
      /* This should not happen. MAXPATHLEN should be large enough. */
      ASSERT(FALSE);
   }
   res = FileIO_Open(&asFd,
                     tempPath,
                     FILEIO_OPEN_ACCESS_READ,
                     FILEIO_OPEN);
   if (res != FILEIO_SUCCESS) {
      Warning("Could not open address space file for pid %"FMT64"d, %s\n",
              (int64_t)pid,
              FileIO_MsgError(res));
      return NULL;
   }

   buf = NULL;
   if (ReadArgsFromAddressSpaceFile(asFd, procInfo, &args)) {
      /* Concatenate the strings in args into cmdLine. */
      DynBuf_Init(&cmdLine);
      argc = DynBufArray_Count(&args);
      for (i = 0; i < argc; i++) {
         buf = DynBuf_Get(DynBufArray_AddressOf(&args, i));
         DynBuf_Append(&cmdLine, buf, strlen(buf));
         DynBuf_Append(&cmdLine, " ", 1);
         DynBuf_Destroy(DynBufArray_AddressOf(&args, i));
      }
      DynBuf_AppendString(&cmdLine,"");
      DynBufArray_Destroy(&args);
      DynBuf_Trim(&cmdLine);
      buf = DynBuf_Detach(&cmdLine);
      DynBuf_Destroy(&cmdLine);
   }
   FileIO_Close(&asFd);
   return buf;
}


/*
 *----------------------------------------------------------------------------
 *
 * ReadArgsFromAddressSpaceFile --
 *
 *      Read the command line arguments for a process and store them in the
 *      cmdLineArr array. The processes' address space file must be open with
 *      the file descriptor adFd. This function assumes that it runs in the
 *      same locale as the process being inspected.
 *
 * Results:
 *
 *      On success, TRUE is returned and the caller is responsible for
 *      de-allocating the memory used by the DynBufArray; by calling
 *      DynBuf_Destroy on each of its elements, and then DynBufArray_Destroy on
 *      the array itself. FALSE is returned on failure, and no de-allocation
 *      is needed.
 *
 * Side effects:
 *
 *      The cmdLineArr array is filled with the command line strings of the
 *      given process.
 *----------------------------------------------------------------------------
 */

static Bool
ReadArgsFromAddressSpaceFile(FileIODescriptor asFd,     //IN
                             psinfo_t *psInfo,          //IN
                             DynBufArray *cmdLineArr)   //OUT
{
   uintptr_t *argOffs;
   uintptr_t argOff;
   uintptr_t nextArgOff;
   int argc;
   int i;
   char *argBuf;
   char *argBufPtr;
   DynBuf *arg;

   argc = psInfo->pr_argc;
   DynBufArray_Init(cmdLineArr, argc);
   for (i = 0; i < argc; i++) {
      DynBuf_Init(DynBufArray_AddressOf(cmdLineArr, i));
   }
   if (argc == 0) {
      return TRUE;
   }
   argOffs = Util_SafeCalloc(argc, sizeof *argOffs);
   if (argOffs == NULL) {
      return FALSE;
   }

   if (!ReadOffsetsFromAddressSpaceFile(asFd, psInfo, argOffs)) {
      goto fail;
   }

   /* Read the command line arguments into the cmdLineArr array. */
   nextArgOff = argc > 0 ? argOffs[0] : 0;
   i = 0;
   while (i < argc) {
      argOff = argOffs[i];

      /*
       * The argument strings are contiguous in the address space file. So
       * argOff[i] + strlen(arg[i]) + 1 should be equal to argOff[i + 1].
       */
      if ((argOff == 0) || (argOff != nextArgOff)) {
         break;
      }
      argBuf = ExtractArgStringFromAddressSpaceFile(asFd, argOff);
      if (argBuf == NULL) {
         goto fail;
      }
      nextArgOff = argOff + strlen(argBuf) + 1;
      argBufPtr = argBuf +  strlen(argBuf) + 1;
      while ((argBufPtr > argBuf) && isspace(*(argBufPtr - 1))) {
         argBufPtr--;
      }
      *argBufPtr = '\0';
      arg = DynBufArray_AddressOf(cmdLineArr, i);
      if (!DynBuf_Append(arg,
                         argBuf,
                         strlen(argBuf) + 1)) {
         free(argBuf);
         goto fail;
      }
      free(argBuf);
      i++;
   }
   return TRUE;

fail:
   Warning("Failed to read command line arguments\n");
   argc = DynBufArray_Count(cmdLineArr);
   for (i = 0; i < argc; i++) {
      arg = DynArray_AddressOf(cmdLineArr, i);
      DynBuf_Destroy(arg);
   }
   DynBufArray_SetCount(cmdLineArr, 0);
   DynBufArray_Destroy(cmdLineArr);
   free(argOffs);
   return FALSE;
}


/*
 *----------------------------------------------------------------------------
 *
 * ReadOffsetsFromAddressSpaceFile --
 *
 *      Read the offsets for the command line arguments strings of a process
 *      into the argOffs array. The processes' /proc/<pid>/as file must be
 *      open with the file descriptor asFd. The argOffs array must have enough
 *      space to store all the offsets for the process.
 *
 * Results:
 *
 *      TRUE on success, FALSE on error.
 *
 * Side Effects:
 *
 *      The argOffs array is filled with the offsets of the command line
 *      arguments.
 *
 *----------------------------------------------------------------------------
 */

static Bool
ReadOffsetsFromAddressSpaceFile(FileIODescriptor asFd, //IN
                                psinfo_t *psInfo,      //IN
                                uintptr_t *argOffs)    //OUT
{
   int argc;
   int i;
   uintptr_t argv;
   FileIOResult res;

   argc = psInfo->pr_argc;
   argv = psInfo->pr_argv;
   /*
    * The offsets for the command line argument are located at an offset of
    * argv in the /proc/<pid>/as file. If the process data model is NATIVE,
    * each offset is a unitptr_t; else if the data model is ILP32 or LP64, each
    * offset is uint32_t.
    */
   if (psInfo->pr_dmodel == PR_MODEL_NATIVE) {
      /*
       * The offset for each arguments is sizeof uintptr_t bytes.
       */
      res = FileIO_Pread(&asFd, argOffs, argc * sizeof argv, argv);
      if (res != FILEIO_SUCCESS) {
         return FALSE;
      }
   } else {
      /*
       * The offset for each arguments is sizeof uint32_t bytes.
       */
      uint32_t *argOffs32;
      argOffs32 = Util_SafeCalloc(argc, sizeof *argOffs32);
      if (argOffs32 == NULL) {
         return FALSE;
      }
      res = FileIO_Pread(&asFd, argOffs32, argc * sizeof *argOffs32, argv);
      if (res != FILEIO_SUCCESS) {
         free (argOffs32);
         return FALSE;
      }
      for (i = 0; i < argc; i++) {
         argOffs[i] = argOffs32[i];
      }
      free(argOffs32);
   }
   return TRUE;
}


/*
 *----------------------------------------------------------------------------
 *
 * ExtractArgStringFromAddressSpaceFile --
 *
 *      Extract a string at a given offset in the given file. The file must be
 *      open with file descriptor asFd.
 *
 * Results:
 *
 *      On success, the NULL terminated string is returned and the
 *      caller is responsible for freeing the memory used by this string. On
 *      failure, NULL is returned.
 *
 * Side effects:
 *
 *      None
 *
 *----------------------------------------------------------------------------
 */

static char *
ExtractArgStringFromAddressSpaceFile(FileIODescriptor asFd,     //IN
                                     uintptr_t offset)          //IN
{
   int readSize = 32;
   char *buf;
   FileIOResult res;

   buf = Util_SafeMalloc(readSize * sizeof *buf);
   while (1) {
      res = FileIO_Pread(&asFd, buf, readSize, offset);
      if (res != FILEIO_SUCCESS) {
         goto fail;
      }
      if (Str_Strlen(buf, readSize) == readSize) {
         readSize *= 2;
         free(buf);
         if (readSize > NCARGS) {
            return NULL;
         }
         buf = Util_SafeMalloc(readSize * sizeof *buf);
      } else {
         break;
      }
   }
   return buf;

fail:
   free(buf);
   return NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * ProcMgr_ImpersonateUserStart --
 *
 *      Impersonate a user.  Much like bora/lib/impersonate, but
 *      changes the real and saved uid as well, to work with syscalls
 *      (access() and kill()) that look at real UID instead of effective.
 *      The user name should be UTF-8 encoded, although we do not enforce
 *      it right now. Solaris does not have setresuid/setresgid. So perform
 *      a two step process to set the real and effective uid/gid to given
 *      user and leave the saved uid/gid as root.
 *
 *      Assumes it will be called as root.
 *
 * Results:
 *      TRUE on success, FALSE on failure
 *
 * Side effects:
 *
 *      Uid/gid set to given user, saved uid/gid left as root.
 *
 *----------------------------------------------------------------------
 */

Bool
ProcMgr_ImpersonateUserStart(const char *user,  // IN: UTF-8 encoded user name
                             AuthToken token)   // IN
{
   char buffer[BUFSIZ];
   struct passwd pw;
   struct passwd *ppw;
   gid_t root_gid;
   int ret;

   ppw = &pw;
   if ((ppw = getpwuid_r(0, &pw, buffer, sizeof buffer)) == NULL) {
      return FALSE;
   }

   root_gid = ppw->pw_gid;

   if ((ppw = getpwnam_r(user, &pw, buffer, sizeof buffer)) == NULL) {
      return FALSE;
   }

   /* first change group. */
   ret = Id_SetGid(root_gid);
   if (ret < 0) {
      Warning("Failed to setregid() for root\n");
      return FALSE;
   }

   /*  Snippet from Solaris setregid man page --
    *
    *  A -1 argument does not change the corresponding gid. If the real user ID
    *  is being changed, or the effective user ID is being changed to a value
    *  not equal to the real user ID, the saved set-user ID is set equal to
    *  the new effective user ID.
    */
   ret = Id_SetREGid(ppw->pw_gid, -1);
   if (ret < 0) {
      Warning("Failed to setregid() for user %s\n", user);
      return FALSE;
   }
   ret = Id_SetREGid(-1, ppw->pw_gid);
   if (ret < 0) {
      Warning("Failed to setregid() for user %s\n", user);
      return FALSE;
   }
   ret = initgroups(ppw->pw_name, ppw->pw_gid);
   if (ret < 0) {
      Warning("Failed to initgroups() for user %s\n", user);
      goto failure;
   }

   /* now user. */
   ret = Id_SetUid(0);
   if (ret < 0) {
      Warning("Failed to setregid() for root\n");
      return FALSE;
   }

   /* Same as above. */
   ret = Id_SetREUid(ppw->pw_uid, -1);
   if (ret < 0) {
      Warning("Failed to setreuid() for user %s\n", user);
      goto failure;
   }
   ret = Id_SetREUid(-1, ppw->pw_uid);
   if (ret < 0) {
      Warning("Failed to setreuid() for user %s\n", user);
      goto failure;
   }

   /* set env. */
   setenv("USER", ppw->pw_name, 1);
   setenv("HOME", ppw->pw_dir, 1);
   setenv("SHELL", ppw->pw_shell, 1);

   return TRUE;

failure:
   /* try to restore on error. */
   ProcMgr_ImpersonateUserStop();
   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * ProcMgr_ImpersonateUserStop --
 *
 *      Stop impersonating a user and return to root. Solaris does not
 *      have setresuid/setresgid. So perform a two step process while
 *      restoring uids to root.
 *
 * Results:
 *      TRUE on success, FALSE on failure.
 *
 * Side effects:
 *
 *      Uid/gid restored to root.
 *
 *----------------------------------------------------------------------
 */

Bool
ProcMgr_ImpersonateUserStop(void)
{
   char buffer[BUFSIZ];
   struct passwd pw;
   struct passwd *ppw;
   int ret;

   ppw = &pw;
   if ((ppw = getpwuid_r(0, &pw, buffer, sizeof buffer)) == NULL) {
      return FALSE;
   }

   /* first change back user, Do the same two step process as above. */
   ret = Id_SetREUid(-1, ppw->pw_uid);
   if (ret < 0) {
      Warning("Failed setreuid() for root\n");
      return FALSE;
   }
   ret = Id_SetREUid(ppw->pw_uid, -1);
   if (ret < 0) {
      Warning("Failed to setreuid() for root\n");
      return FALSE;
   }

   /* now group. */
   ret = Id_SetGid(ppw->pw_gid);
   if (ret < 0) {
      Warning("Failed to setgid() for root\n");
      return FALSE;
   }
   ret = initgroups(ppw->pw_name, ppw->pw_gid);
   if (ret < 0) {
      Warning("Failed to initgroups() for root\n");
      return FALSE;
   }

   /* set env. */
   setenv("USER", ppw->pw_name, 1);
   setenv("HOME", ppw->pw_dir, 1);
   setenv("SHELL", ppw->pw_shell, 1);

   return TRUE;
}