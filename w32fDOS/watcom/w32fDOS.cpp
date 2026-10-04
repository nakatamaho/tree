/****************************************************************************

  Win32 File compatibility for DOS: Open Watcom version.

  Translation of w32fDOS/small/w32fDOS.cpp (Kenneth J. Davis, August 2000,
  public domain) for Open Watcom, which has no Borland pseudo registers or
  asm blocks. Each Borland asm sequence is replaced by an intdosx() call
  with the same INT 21h function, registers and DTA handling.

  Borland sets the carry flag (STC) before the LFN calls; a DOS without LFN
  support may return AX=7100h and leave the carry unchanged. Open Watcom
  intdosx clears the carry before the call, so AX=7100h after an LFN call is
  treated as the carry set (lfn_failed()), which gives the Borland result.

  Expects near data pointers (tiny, small and medium models).

Copyright (c): Public Domain [United States Definition]

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR AUTHORS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

****************************************************************************/

#include "w32fdos.h"
#include <stdlib.h>
#include <string.h>
#include <dos.h>

#define searchAttr ( FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN | \
   FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE )

/* See w32fDOS/small/w32fDOS.cpp. */
int LFN_Enable_Flag = LFN_ENABLE;

/* Carry after an LFN call, counting AX=7100h as "unsupported" (see above). */
static int lfn_failed(const union REGS *r)
{
  return r->x.cflag || r->x.ax == 0x7100;
}

/* INT 21h with DS and ES set to our (near) data segment. */
static void dos_call(union REGS *r)
{
  struct SREGS s;
  segread(&s);
  s.es = s.ds;
  intdosx(r, r, &s);
}

/* Run INT 21h AH=4Eh/4Fh with the DTA temporarily set to dta.
   Returns 0 on success, else the DOS error code (as the Borland cflag). */
static short find_with_dta(FFDTA *dta, unsigned short function,
                           const char *path, unsigned short attrib)
{
  union REGS r;
  struct SREGS s;
  unsigned short old_off, old_seg;
  short result = 0;

  segread(&s);
  r.h.ah = 0x2F;                      /* get current DTA, returned in ES:BX */
  intdosx(&r, &r, &s);
  old_off = r.x.bx;
  old_seg = s.es;

  segread(&s);
  r.h.ah = 0x1A;                      /* set current DTA to our buffer */
  r.x.dx = (unsigned short)dta;
  intdosx(&r, &r, &s);

  segread(&s);
  r.x.ax = function;                  /* findfirst or findnext */
  r.x.cx = attrib;
  r.x.dx = (unsigned short)path;
  intdosx(&r, &r, &s);
  if (r.x.cflag)
    result = r.x.ax;

  segread(&s);
  s.ds = old_seg;                     /* set current DTA back to original */
  r.h.ah = 0x1A;
  r.x.dx = old_off;
  intdosx(&r, &r, &s);
  return result;
}

/* copy old style findfirst data FFDTA to a WIN32_FIND_DATA
 * NOTE: does not map exactly.
 */
static void copyFileData(WIN32_FIND_DATAA *findData, FFDTA *finfo)
{
  strcpy(findData->cFileName, (char *)finfo->ff_name);
  findData->dwFileAttributes = (DWORD)finfo->ff_attrib;

  findData->ftCreationTime.ldw[0] = finfo->ff_ftime;
  findData->ftLastAccessTime.ldw[0] = finfo->ff_ftime;
  findData->ftLastWriteTime.ldw[0] = finfo->ff_ftime;
  findData->ftCreationTime.ldw[1] = finfo->ff_fdate;
  findData->ftLastAccessTime.ldw[1] = finfo->ff_fdate;
  findData->ftLastWriteTime.ldw[1] = finfo->ff_fdate;
  findData->ftCreationTime.hdw = 0;
  findData->ftLastAccessTime.hdw = 0;
  findData->ftLastWriteTime.hdw = 0;
  findData->nFileSizeHigh = 0;
  findData->nFileSizeLow = (DWORD)finfo->ff_fsize;
  findData->dwReserved0 = 0;
  findData->dwReserved1 = 0;
  findData->cAlternateFileName[0] = '\0';
}

HANDLE FindFirstFileA(const char *pathname, WIN32_FIND_DATAA *findData)
{
  char path[1024];
  HANDLE hnd;
  union REGS r;
  short cflag;

  if (findData == NULL)
    return INVALID_HANDLE_VALUE;

  if ((hnd = (HANDLE)malloc(sizeof(struct FindFileStruct))) == NULL)
    return INVALID_HANDLE_VALUE;
  memset(hnd, 0, sizeof(struct FindFileStruct));

  /* Clear findData, this is to fix a glitch under NT, with 'special' $???? files */
  memset(findData, 0, sizeof(struct WIN32_FIND_DATA));

  /* First try DOS LFN (0x714E) findfirst, going to old (0x4E) if error */
  if (LFN_Enable_Flag)
  {
    hnd->flag = FINDFILELFN;
    r.x.ax = 0x714E;
    r.x.cx = searchAttr;
    r.x.si = 1;                       /* same date/time format as old style */
    r.x.dx = (unsigned short)pathname;
    r.x.di = (unsigned short)findData;
    dos_call(&r);
    if (!lfn_failed(&r))
    {
      hnd->fhnd.handle = r.x.ax;      /* store handle */
      return hnd;
    }
    /* AX is 7100h if not supported, FreeDOS may return 1 instead */
    if ((r.x.ax != 0x7100) && (r.x.ax != 0x0001))
    {
      free(hnd);
      return INVALID_HANDLE_VALUE;
    }
  }

  /* Use DOS (0x4E) findfirst, returning if error */
  hnd->flag = FINDFILEOLD;

  if ((hnd->fhnd.ffdtaptr = (FFDTA *)malloc(sizeof(struct FFDTA))) == NULL)
  {
    free(hnd);
    return INVALID_HANDLE_VALUE;
  }

  /* if pathname ends in \* convert to \*.* */
  strcpy(path, pathname);
  int eos = strlen(path);
  eos--;
  if (path[eos] == '*')
    if (path[eos-1] == '\\')
      strcat(path, ".*");

  cflag = find_with_dta(hnd->fhnd.ffdtaptr, 0x4E00, path, searchAttr);
  if (cflag)
  {
    free(hnd->fhnd.ffdtaptr);
    free(hnd);
    return INVALID_HANDLE_VALUE;
  }

  copyFileData(findData, hnd->fhnd.ffdtaptr);
  return hnd;
}


int FindNextFileA(HANDLE hnd, WIN32_FIND_DATAA *findData)
{
  union REGS r;

  if ((hnd == NULL) || (hnd == INVALID_HANDLE_VALUE)) return 0;
  if (findData == NULL) return 0;

  memset(findData, 0, sizeof(struct WIN32_FIND_DATA));

  if (hnd->flag == FINDFILELFN)
  {
    r.x.ax = 0x714F;                  /* LFN version of FindNext */
    r.x.bx = hnd->fhnd.handle;
    r.x.si = 1;
    r.x.di = (unsigned short)findData;
    dos_call(&r);
    return lfn_failed(&r) ? 0 : 1;
  }

  /* Use DOS (0x4F) findnext; the DTA keeps the search state */
  if (find_with_dta(hnd->fhnd.ffdtaptr, 0x4F00, "", 0))
    return 0;

  copyFileData(findData, hnd->fhnd.ffdtaptr);
  return 1;
}


/* free resources to prevent memory leaks */
void FindClose(HANDLE hnd)
{
  union REGS r;

  if ((hnd != NULL) && (hnd != INVALID_HANDLE_VALUE))
  {
    if (hnd->flag == FINDFILEOLD)
    {
      if (hnd->fhnd.ffdtaptr != NULL)
        free(hnd->fhnd.ffdtaptr);
      hnd->fhnd.ffdtaptr = NULL;
    }
    else
    {
      r.x.ax = 0x71A1;                /* LFN findclose, carry set on error */
      r.x.bx = hnd->fhnd.handle;
      dos_call(&r);
      hnd->fhnd.handle = 0;
    }
    free(hnd);
  }
}


/* See w32fDOS/small/w32fDOS.cpp for the description; only the first four
 * arguments are used and zero is returned on failure. */
int GetVolumeInformation(char *lpRootPathName,char *lpVolumeNameBuffer,
  DWORD nVolumeNameSize, DWORD *lpVolumeSerialNumber,
  DWORD *lpMaximumComponentLength, DWORD *lpFileSystemFlags,
  char *lpFileSystemNameBuffer, DWORD nFileSystemNameSize)
{
  FFDTA finfo;
  struct media_info
  {
    short dummy;
    DWORD serial;
    char volume[11];
    short ftype[8];
  } media;
  char fsystem[32];
  char pathname[260];
  union REGS r;
  /* 0 = success, 1 = failure, 2 = LFN api unsupported (tentative failure) */
  int cflag = 2;

  (void)lpMaximumComponentLength; (void)lpFileSystemFlags;
  (void)lpFileSystemNameBuffer; (void)nFileSystemNameSize;

  if ((lpRootPathName == NULL) || (*lpRootPathName == '\0'))
  {
    r.h.ah = 0x19;                    /* current default drive, 0=A */
    intdos(&r, &r);
    pathname[0] = (char)('A' + r.h.al);
    pathname[1] = ':';
    pathname[2] = '\\';
    pathname[3] = '\0';
  }
  else
    strcpy(pathname, lpRootPathName);

  if (LFN_Enable_Flag)
  {
    r.x.ax = 0x71A0;                  /* LFN GetVolumeInformation */
    r.x.di = (unsigned short)fsystem;
    r.x.cx = sizeof(fsystem);
    r.x.dx = (unsigned short)pathname;
    dos_call(&r);
    if (!lfn_failed(&r))
      cflag = 0;
    else if ((r.x.ax != 0x7100) && (r.x.ax != 0x0001))
      cflag = 1;
  }

  if (cflag != 1)
  {
    if (pathname[1] == ':')
    {
      cflag = 0;

      if (pathname[strlen(pathname)-1] != '\\')
        strcat(pathname, "\\*.*");
      else
        strcat(pathname, "*.*");

      /* Search for the volume label with the old findfirst; a true volume
         entry has only the label attribute (archive bit ignored). */
      cflag = find_with_dta(&finfo, 0x4E00, pathname, FILE_ATTRIBUTE_LABEL);
      while (!cflag && (finfo.ff_attrib & 0xDF) != FILE_ATTRIBUTE_LABEL)
        cflag = find_with_dta(&finfo, 0x4F00, "", 0);

      if (lpVolumeNameBuffer != NULL)
      {
        if (cflag != 0)
          lpVolumeNameBuffer[0] = '\0';
        else
        {
          strncpy(lpVolumeNameBuffer, (char *)finfo.ff_name, (size_t)nVolumeNameSize);
          lpVolumeNameBuffer[nVolumeNameSize-1] = '\0';
          if (lpVolumeNameBuffer[8] == '.')
          {
            lpVolumeNameBuffer[8] = lpVolumeNameBuffer[9];
            lpVolumeNameBuffer[9] = lpVolumeNameBuffer[10];
            lpVolumeNameBuffer[10] = lpVolumeNameBuffer[11];
            lpVolumeNameBuffer[11] = '\0';
          }
        }
      }
      /* No more files / file not found: no label, still a valid drive */
      if ((cflag == 0x12) || (cflag == 0x02))
        cflag = 0;

      /* Serial number through generic IOCTL 440Dh CX=0866h; stays 0 on error */
      media.serial = 0;
      r.x.ax = 0x440D;
      r.x.bx = (pathname[0] & '\xDF') - 'A' + 1;
      r.x.cx = 0x0866;
      r.x.dx = (unsigned short)&media;
      dos_call(&r);

      if (lpVolumeSerialNumber != NULL)
        *lpVolumeSerialNumber = media.serial;
    }
    else
    {
      if (lpVolumeNameBuffer != NULL)
        lpVolumeNameBuffer[0] = '\0';
      if (lpVolumeSerialNumber != NULL)
        *lpVolumeSerialNumber = 0x0;
    }
  }

  return cflag ? 0 : 1;
}
