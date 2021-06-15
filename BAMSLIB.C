/*
 * BAMSIB.c
 *
 *
 * (C) 1995 Info Tech Inc.
 *
 * Craig Fitzgerald
 *
 * This file is part of the BAMS Library module
 *
 * This is the main file for the BAMSib program.  This file provides the
 * functionality for handling library files.
 *
 */


#include <os2.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <io.h>
#include <conio.h>
#include <GnuMem.h>
#include <GnuArg.h>
#include <GnuStr.h>
#include <GnuFile.h>
#include <GnuZip.h>
#include <GnuMisc.h>
#include "ReadBAMS.h"
#include "BAMSLib.h"

#define TEMPLIB       "TMP--LIB.TMP"

#define BUFFERSIZE    35000U
#define MAXDESCLEN    2000U

#define TIME          __TIME__
#define DATED         __DATE__

#define LIB           1
#define CMDLINE       2
#define UPDATE        3
#define DELET         4
#define EXTRACT       5

#define INITCRC       12345L

//  BAMS file format:
//
//
//  LibHeader
//  LibDescriptor
//  FileDescriptor
//  FileData
//  FileDescriptor
//  FileData
//  FileDescriptor
//  FileData
//  FileDescriptor
//  FileData
//  .
//  .
//  .
//
//  LibHeader:
//     char [30]    text label
//
//  LibDescriptor:
//     ULONG   ulOffset    Offset to 1st FileDescriptor
//     ULONG   ulSize      ?
//     USHORT  uCount      ?
//     USHORT  uLibVer     Library Version
//     char[]  pszDesc     Library Description
//
//  FileDescriptor:
//     ULONG   ulMark      Integrity mark
//     ULONG   ulOffset    Offset to FileData
//     ULONG   ulLen       Length of original data
//     ULONG   ulSize      Size of compressed data
//     ULONG   ulCRC       CRC of data
//     USHORT  uMethod     Compression method
//     USHORT  fDate       File Date
//     USHORT  fTime       File Time
//     USHORT  uAtt        File Attributes
//     char[]  szName      File Name
//     char[]  pszDesc     File Description
//
//  FileData:
//     ULONG   ulMark      Integrity Mark
//     char[]  szData      Data, maybe compressed
//


/*
 * This is from EblibTxt.DAT
 *
 */
extern char szUsage1[];
extern char szUsage2[];
extern char szUsage3[];


void Usage (USHORT i);

char szLib [256];

PFDESC fList = NULL;
FDESC fdesc;

int  iOVERWRITE = 0;

PSZ  pszWorkBuff = NULL;

BOOL bDEBUGMODE;
BOOL bSTOREONLY;
BOOL bINCLSYSTEM;
BOOL bINCLHIDDEN;
BOOL bLIBDESC;
BOOL bFILEDESC;

FTIME ftimeZero = {0, 0, 0};
FDATE fdateZero = {0, 0, 0};


/*******************************************************************/
/*                                                                 */
/*                                                                 */
/*                                                                 */
/*******************************************************************/


USHORT Err (PSZ p1, PSZ p2)
   {
   printf ("BAMSIB: ");
   printf (p1, p2);
   printf ("\n");
   exit (1);
   return 1;
   }


void WriteMark (FILE *fp)
   {
   FilWriteLong (fp, BAMSMARK);
   }


int NewStrLen (PSZ p)
   {
   if (!p)
      return 0;
   return strlen (p);
   }





BOOL touch (PSZ pszFileName, FDATE fdate, FTIME ftime)
   {
   HFILE      hFile;
   FILESTATUS fstsBuf;
   USHORT     uAction;

   if (DosOpen (pszFileName, &hFile, &uAction, 0UL,
                FILE_NORMAL, FILE_OPEN, 
                OPEN_ACCESS_READWRITE | OPEN_SHARE_DENYNONE, 0UL))
      return FALSE;

   if (DosQFileInfo (hFile, 0x0001, &fstsBuf, sizeof fstsBuf))
      return DosClose (hFile);

   /*--- we should only have to change the last write date ---*/
   fstsBuf.fdateLastWrite  = fdate;
   fstsBuf.ftimeLastWrite  = ftime;
   fstsBuf.fdateCreation   = fdateZero;
   fstsBuf.ftimeCreation   = ftimeZero;
   fstsBuf.fdateLastAccess = fdateZero;
   fstsBuf.ftimeLastAccess = ftimeZero;

   if (DosSetFileInfo (hFile, 0x0001, (PVOID)&fstsBuf, sizeof fstsBuf))
      return DosClose (hFile);

   return !DosClose (hFile);
   }



/*******************************************************************/
/*                                                                 */
/* Compression / Uncompression routines                            */
/*                                                                 */
/*******************************************************************/


/*
 * Here is what is going on:
 *   This fn accomplishes 2 tasks:
 *      1> copy data from 1 file to another (or NULL)
 *      2> generate a CRC value from the data
 *   Generating a CRC value of uncompressed data is simple, just
 *   get a cumulitave CRC value of each byte in the stream.
 *   Generating a CRC value of compressed data is not so simple
 *   beacuse is format is:
 *      CompressedSegmentLength  (2 bytes, call it n)
 *      CompressedSegment        (n-2 bytes)
 *      CompressedSegmentLength  (2 bytes, call it m)
 *      CompressedSegment        (m-2 bytes)
 *      .                        .
 *      .                        .
 *      .                        .
 *   We need to get a cumulative CRC of the CompressedSegment's only.
 *   Added to this, this fn buffers the input/output. This buffer
 *   area (of size BUFFERSIZE), is not necessarily as large as a
 *   CompressedSegment, so each segment may require more than 1 read
 *
 *   The reason for the complicated CRC on the compressed data is one
 *   of speed. The compression generates a CRC as it writes, but doesn't
 *   write the CompressedSegmentLength until the segment is written.
 *
 *   Why not generate a CRC of the data in uncompressed form you say?
 *   Again for Speed.  Using a crc for the compressed form greatly speeds
 *   up file validation, and allows verification of all files in a lib
 *   when the lib is being updated
 *
 * returns:
 * 0: ok
 * 1: unexpected eof on input
 * 2: unexpected eof on output
 * 5: file size error
 *
 */

USHORT CopyFile (FILE *fpIn, FILE *fpOut, ULONG ulSize, USHORT uCompression)
   {
   USHORT uPiece, uIOBytes;
   ULONG  ulSegment;

   while (ulSize)
      {
      if (uCompression)
         {
         if ((ulSegment = (ULONG)(USHORT)FilReadShort (fpIn)) > ulSize)
            return 5;

         ulSize -= ulSegment;

         if (fpOut)
            FilWriteShort (fpOut, (USHORT)ulSegment);
         ulSegment -= 2;
         }
      else
         {
         ulSegment = ulSize;
         ulSize = 0;
         }

      while (ulSegment)
         {
         uPiece = (USHORT) min ((ULONG)BUFFERSIZE, ulSegment);
         uIOBytes = fread (pszWorkBuff, 1, uPiece, fpIn);
         if (uPiece != uIOBytes)
            return 1;

         if (fpOut)
            {
            uIOBytes = fwrite (pszWorkBuff, 1, uPiece, fpOut);
            if (uPiece != uIOBytes)
               return 2;
            }
         ulSegment -= uPiece;

         if (bGENWRITECRC)
            ulWRITECRC = CRC_BUFF (ulWRITECRC, pszWorkBuff, uPiece);
         if (bGENREADCRC)
            ulREADCRC = CRC_BUFF (ulREADCRC, pszWorkBuff, uPiece);
         }
      }
   return 0;
   }




/*
 * returns:
 *  0 - ok
 *  5 - file size error
 */
USHORT UncompressFile (FILE *fpIn, FILE *fpOut, ULONG ulInSize, ULONG ulOutSize)
   {
   USHORT uInSize, uOutSize;
   ULONG  ulSrcRead, ulTotalOut;

   ulSrcRead = ulTotalOut = 0;

   while (ulSrcRead < ulInSize)
      {
      Cmp2fpEfp (fpOut, &uOutSize, fpIn, &uInSize);
      ulSrcRead  += uInSize;
      ulTotalOut += uOutSize;
      }
   if (ulTotalOut != ulOutSize)
      {
      printf ("Size Err: Expected:%ld  Got:%ld\n", ulOutSize, ulTotalOut);
      return 1;
      }
   return 0;
   }


/*
 * no error return
 */
USHORT CompressFile (FILE *fpIn, FILE *fpOut, ULONG ulInSize, PULONG pulOutSize)
   {
   USHORT uInSize, uOutSize, uChunkSize;
   ULONG  ulTotalIn, ulTotalOut;

   ulTotalIn = ulTotalOut = 0;

   while (ulTotalIn < ulInSize)
      {
      uChunkSize = 0;
      if (ulInSize - ulTotalIn < 65536)
         uChunkSize = (USHORT)(ulInSize - ulTotalIn);

      Cmp2fpIfp (fpOut, &uOutSize, fpIn, uChunkSize, &uInSize);
      ulTotalIn  += uInSize;
      ulTotalOut += uOutSize;
      }
   *pulOutSize = ulTotalOut;
   return 0;
   }



/*******************************************************************/
/*                                                                 */
/*                                                                 */
/*                                                                 */
/*******************************************************************/


BOOL MatchesParams (PSZ pszName, BOOL bMatchIfNoParams)
   {
   USHORT i, uParams;
   PSZ    pszParam;

   if ((uParams = ArgIs (NULL)) < 2)
      return bMatchIfNoParams;

   for (i = 1; i < uParams; i++)
      {
      pszParam = ArgGet (NULL, i);

      if (StrMatches (pszName, pszParam, FALSE))
         return i;
      }
   return FALSE;
   }



PSZ GetLibDesc (PSZ pszBuff, PSZ pszDefaultDesc)
   {
   FILE   *fpDesc;
   USHORT uLen;
   PSZ    pszFile;

   *pszBuff = '\0';

   if (!bLIBDESC)
      return pszDefaultDesc;

   if (!(pszFile = ArgGet ("i", 0)))
      return NULL;

   /*--- read new description from file ---*/
   if (!(fpDesc = fopen (pszFile, "rt")))
      Err ("Error: Unable to open Description file: %s", pszFile);
   uLen = fread (pszBuff, 1, MAXDESCLEN, fpDesc);
   pszBuff [uLen] = '\0';
   fclose (fpDesc);
   return pszBuff;
   }


/*******************************************************************/
/*                                                                 */
/*                                                                 */
/*                                                                 */
/*******************************************************************/


/*
 * fp should point to the file data area
 * 0 - ok
 * 1 - unexpected eof on input
 * 2 - unexpected eof on output
 * 3 - crc error
 */
USHORT TestFile (PFDESC pfd)
   {
   USHORT uRet;

   ReadMark (pfd->pld->fp);

   /*--- compression module vars ---*/
   bGENREADCRC  = TRUE;
   bGENWRITECRC = FALSE;
   ulREADCRC    = INITCRC;

   if (uRet = CopyFile (pfd->pld->fp, NULL, pfd->ulSize, pfd->uMethod))
      return uRet;
   if (ulREADCRC != pfd->ulCRC)
      return 3;
   return 0;
   }



/*
 * This fn writes a file's data from a lib to a file
 * This fn assumes the file pointer is pointing
 * to the start of the file data area unless bSetFilePos 
 */
USHORT WriteToFile (PFDESC pfd, BOOL bSetFilePos)
   {
   FILE   *fpOut;
   int    c;
   USHORT uErr;

   if (!access (pfd->szName, 0))
      {
      if (!iOVERWRITE)
         {
         printf ("File %s exists. Overwrite ? [ynYN] ", pfd->szName);
         while (kbhit ())
            getch ();
         c = getch ();
         putchar ('\n');
         if (c == 'Y')
            iOVERWRITE = 1;
         else if (c == 'N')
            iOVERWRITE = -1;
         else if (c == 'n')
            return FALSE;
         }
      if (iOVERWRITE < 0)
         return FALSE;
      }
   if (!(fpOut = fopen (pfd->szName, "wb")))
      {
      printf ("BAMSIB: can't open file: %s\n", pfd->szName);
      return FALSE;
      }

   if (bSetFilePos)
      fseek (pfd->pld->fp, pfd->ulOffset, SEEK_SET);

   ReadMark (pfd->pld->fp);

   printf (" %s file: %s", (pfd->uMethod ? "  UnHosing" : "Extracting"), pfd->szName);

   /*--- compression module vars ---*/
   bGENREADCRC  = TRUE;
   bGENWRITECRC = FALSE;
   ulREADCRC    = INITCRC;

   if (pfd->uMethod)
      uErr = UncompressFile (pfd->pld->fp, fpOut, pfd->ulSize, pfd->ulLen);
   else
      uErr = CopyFile (pfd->pld->fp, fpOut, pfd->ulSize, pfd->uMethod);

   if (ulREADCRC == pfd->ulCRC)
      printf ("\n");
   else
      printf (" fails CRC check.\n");

   fclose (fpOut);

   /*--- set date / time ---*/
   touch (pfd->szName, pfd->fDate, pfd->fTime);

   /*--- set file mode ---*/
   DosSetFileMode (pfd->szName, pfd->uAtt, 0);

   /*-- write 4dos descriptions --*/
   FilPut4DosDesc (pfd->szName, pfd->szDesc);

   return uErr;
   }


int CompareFileNames (PSZ pszA, PSZ pszB)
   {
   PSZ psz;

   pszA = ((psz = strrchr (pszA, ':'))  ? psz+1 : pszA);
   pszA = ((psz = strrchr (pszA, '\\')) ? psz+1 : pszA);
   pszB = ((psz = strrchr (pszB, ':'))  ? psz+1 : pszB);
   pszB = ((psz = strrchr (pszB, '\\')) ? psz+1 : pszB);
   return stricmp (pszA, pszB);
   }


/*
 * add files from old lib first
 * add cmd line files next
 *
 * MODES:
 *   LIB
 *   CMDLINE
 *   UPDATE
 */
void AddToFileList (PFDESC pfd)
   {
   PFDESC fTmp;
   int    i;

   pfd->Next = NULL;

   /*--- 1st node ? ---*/
   if (!fList)
      {
      fList = pfd;
      return;
      }
   /*--- before 1st node ? ---*/
   if ((i = CompareFileNames (fList->szName, pfd->szName)) > 0)
      {
      pfd->Next = fList;
      fList = pfd;
      return;
      }
   /*--- does it match the first node ? ---*/
   else if (!i)
      {
      if (fList->uMode == LIB)
         pfd->uMode = UPDATE;
      fTmp = fList;
      pfd->Next = fList->Next;
      fList = pfd;
      free (fTmp);
      return;
      }

   for (fTmp = fList; fTmp; fTmp = fTmp->Next)
      {
      if (!fTmp->Next || (i = CompareFileNames (fTmp->Next->szName, pfd->szName)) > 0)
         {
         pfd->Next = fTmp->Next;
         fTmp->Next = pfd;
         break;
         }
      else if (!i)
         {
         if (fTmp->Next->uMode == LIB)
            pfd->uMode = UPDATE;
         pfd->Next = fTmp->Next->Next;
         free (fTmp->Next);
         fTmp->Next = pfd;
         break;
         }
      }
   }



void WriteFileHeader (FILE *fpOut, PFDESC pfd)
   {
   PUSHORT p;
   PSZ     psz, psz2;

   WriteMark  (fpOut);
   FilPushPos (fpOut);
   FilWriteLong  (fpOut, 0);               // ulOffset
   FilWriteLong  (fpOut, pfd->ulLen);
   FilWriteLong  (fpOut, pfd->ulSize);
   FilWriteLong  (fpOut, pfd->ulCRC);
   FilWriteShort (fpOut, pfd->uMethod);
   p = (PUSHORT)(PVOID)&(pfd->fDate);
   FilWriteShort (fpOut, *p);
   p = (PUSHORT)(PVOID)&(pfd->fTime);
   FilWriteShort (fpOut, *p);
   FilWriteShort (fpOut, pfd->uAtt);

   /*--- strip off any path stuff---*/
   psz = ((psz2 = strrchr (pfd->szName, ':')) ? psz2+1 : pfd->szName);
   psz = ((psz2 = strrchr (psz, '\\')) ? psz2+1 : psz);
   FilWriteStr   (fpOut, psz);

   FilWriteStr   (fpOut, pfd->szDesc);
   FilSwapPos (fpOut, TRUE);
   FilWriteLong (fpOut, FilPeekPos (fpOut));
   FilPopPos (fpOut, TRUE);
   }



void UpdateFileHeader (FILE *fp, ULONG ulFilePos, ULONG ulOutSize, ULONG ulLen, ULONG ulWRITECRC)
   {
   FilPushPos (fp);
   fseek (fp, ulFilePos + SIZEOFFSET, SEEK_SET);
   FilWriteLong (fp, ulOutSize);
   FilWriteLong (fp, ulWRITECRC);
   FilPopPos (fp, TRUE);
   printf ("(%2.2u%%), Done.\n", Ratio (ulOutSize, ulLen));
   }



/*
 * adds a file to the library
 *
 * returns:
 * 0   = ok
 * 100 = file got bigger ?
 * 2   = write error
 * 3   = compression error
 */

USHORT WriteTheDamnFile (PLDESC pldOut, PFDESC pfd, ULONG ulFilePos)
   {
   FILE   *fpOut, *fpIn;
   ULONG  ulOutSize;
   USHORT uErr;

   /*--- compression module vars ---*/
   bGENREADCRC  = FALSE;
   bGENWRITECRC = TRUE;
   ulWRITECRC   = INITCRC;

   fpOut = pldOut->fp;

   WriteFileHeader (fpOut, pfd);
   WriteMark  (fpOut);

   if (pfd->uMode == LIB)
      {
      fseek (pfd->pld->fp, pfd->ulOffset, SEEK_SET);
      ReadMark (pfd->pld->fp);
      uErr = CopyFile (pfd->pld->fp, fpOut, pfd->ulSize, pfd->uMethod);
      return uErr;
      }

   if (pfd->uMode != UPDATE && pfd->uMode != CMDLINE)
      return 0;

   printf (pfd->uMethod ? " Hosing " : "Storing ");

   if (!(fpIn = fopen (pfd->szName, "rb")))
      {
      printf ("Could not open file\n");
      return 2;
      }

   if (!pfd->uMethod)
      {
      if (!(uErr = CopyFile (fpIn, fpOut, pfd->ulSize, pfd->uMethod)))
         UpdateFileHeader (fpOut, ulFilePos, pfd->ulSize, pfd->ulSize, ulWRITECRC);

      fclose (fpIn);
      return uErr;
      }

   uErr = CompressFile (fpIn, fpOut, pfd->ulSize, &ulOutSize);
   fclose (fpIn);

   /*--- file got bigger ---*/
   if (!uErr && ulOutSize > pfd->ulSize)
      {
      printf ("\b\b\b\b\b\b\b");
      return 100;
      }

   if (!uErr)
      UpdateFileHeader (fpOut, ulFilePos, ulOutSize, pfd->ulSize, ulWRITECRC);
   return uErr;
   }



void WriteLibFromList (PLDESC pldOut)
   {
   PFDESC pfd;
   ULONG  ulFilePos, ulCurrPos;
   USHORT i, uRet, iFiles = 0;

   for (i=0, pfd = fList; pfd; pfd = pfd->Next, i++)
      {
      if (pfd->uMode == DELET)
         {
         printf ("  Deleting: %12s\n", pfd->szName);
         continue;
         }

      if (pfd->uMode == CMDLINE)
         printf ("    Adding: %-12s  ", pfd->szName);
      else if (pfd->uMode == UPDATE)
         printf ("  Updating: %-12s  ", pfd->szName);

      ulFilePos = ftell (pldOut->fp);

      if (uRet = WriteTheDamnFile (pldOut, pfd, ulFilePos))
         {
         fseek (pldOut->fp, ulFilePos, SEEK_SET); // rewind over error'd file

         if (uRet == 100)                         // bloating error? try again
            {
            pfd->uMethod = STORE;
            if (uRet = WriteTheDamnFile (pldOut, pfd, ulFilePos))
               {
               fseek (pldOut->fp, ulFilePos, SEEK_SET);
               }
            else
               iFiles++;
            }
         }
      else
         iFiles++;
      }
   WriteMark  (pldOut->fp);
   ulCurrPos = ftell (pldOut->fp);
   fseek (pldOut->fp, FILELENOFFSET, SEEK_SET);
   FilWriteLong  (pldOut->fp, ulCurrPos);
   FilWriteShort (pldOut->fp, iFiles);
   fclose (pldOut->fp);
   }



void DeleteFilesInList (void)
   {
   PFDESC pfd;

   for (pfd = fList; pfd; pfd = pfd->Next) 
      if (pfd->uMode == CMDLINE || pfd->uMode == UPDATE)
         unlink (pfd->szName);
   }



/*******************************************************************/
/*                                                                 */
/*                                                                 */
/*                                                                 */
/*******************************************************************/




/*
 * fp is left at end, where 1st MARK will be placed
 */
PLDESC MakeLibFile (PSZ pszLib, PSZ pszDesc)
   {
   PLDESC pld;

   pld = malloc (sizeof (LDESC));

   pld->ulOffset = HEADERSIZE + 13 + NewStrLen (pszDesc);
   pld->ulSize   = 0;
   pld->uCount   = 0;
   pld->uLibVer  = LIBVER;
   pld->pszDesc  = (pszDesc ? strdup (pszDesc) : NULL);

   if (!(pld->fp = fopen (pszLib, "wb")))
      Err ("can't create: %s", pszLib);

   fwrite (LIBHEADER, 1, HEADERSIZE, pld->fp);
   FilWriteLong  (pld->fp, pld->ulOffset);
   FilWriteLong  (pld->fp, pld->ulSize  );
   FilWriteShort (pld->fp, pld->uCount  );
   FilWriteShort (pld->fp, pld->uLibVer );
   FilWriteStr   (pld->fp, pld->pszDesc );

   return pld;
   }


/*******************************************************************/
/*                                                                 */
/*                                                                 */
/*                                                                 */
/*******************************************************************/





int ListLib (PSZ pszLib, BOOL bShowDescriptions)
   {
   PLDESC pld;
   PFDESC pfd;
   USHORT i, uParams, j;
   ULONG  ulTotLen, ulTotSize;

   ulTotLen = ulTotSize = 0;

   uParams = ArgIs (NULL);

   if (!(pld = OpenLib (pszLib)))
      Err ("Error: %s \n", szLIBERR);

   if (pld->pszDesc && *pld->pszDesc)
      {
      putchar ('\n');
      fputs (pld->pszDesc, stdout);
      putchar ('\n');
      putchar ('\n');
      }

   if (!pld->uCount)
      {
      printf ("No files found.");
      return 0;
      }

   if (bShowDescriptions)
      {
      printf ("Name         Size&Ratio Date      Time   Description\n");
      printf ("----         ---------- ----      ----   -----------\n");
      }
   else
      {
      printf ("  Length  Method   Size  Ratio   Date    Time    Att   Name\n");
      printf ("  ------  ------   ----  -----   ----    ----    ---   ----\n");
      }

   for (i=j=0; i<pld->uCount; i++)
      {
      if (!(pfd = ReadFileInfo (pld, TRUE)))
         Err ("Error: %s \n", szLIBERR);

      if (!MatchesParams (pfd->szName, TRUE))
         continue;

      j++;

      if (bShowDescriptions)
         {
         printf ("%-12s%5luk(%2lu%%) %s %s %.38s\n",
               pfd->szName,
               (pfd->ulLen+512UL) / 1024UL,
               Ratio (pfd->ulSize, pfd->ulLen),
               DateStr (pfd->fDate),
               TimeStr (pfd->fTime),
               pfd->szDesc);
         }
      else
         {
         printf ("%8lu  %6s%8lu %3lu%%  %s %5s  %s  %s\n",
            pfd->ulLen,          (pfd->uMethod == HOSE ? "Hosed " : "Stored"),
            pfd->ulSize,          Ratio (pfd->ulSize, pfd->ulLen),
            DateStr (pfd->fDate), TimeStr (pfd->fTime), AttStr (pfd->uAtt),
            pfd->szName);
         }

      ulTotLen  += pfd->ulLen;
      ulTotSize += pfd->ulSize;
      }

   if (!j)
      {
      printf ("No matching files found.");
      return 0;
      }

   if (bShowDescriptions)
      {
      printf ("----         ----------\n");
      printf ("%3u         %5luk(%2lu%%)\n", j, (ulTotLen+512UL) / 1024LU, Ratio (ulTotSize, ulTotLen));
      }
   else
      {
      printf ("  ------          ------  ---                          -------\n");
      printf ("%8lu        %8lu %3lu%%                             %4u\n",
              ulTotLen, ulTotSize, Ratio (ulTotSize, ulTotLen), j);
      }

   fclose (pld->fp);
   return 0;
   }





int TestLib (PSZ pszLib)
   {
   PLDESC pld;
   PFDESC pfd;
   PSZ    p;
   USHORT uRet, i;

   if (!(pld = OpenLib (pszLib)))
      Err ("Error: %s \n", szLIBERR);

   if (!pld->uCount)
      {
      printf ("No files found.");
      return 0;
      }

   for (i=0; i<pld->uCount; i++)
      {
      if (!(pfd = ReadFileInfo (pld, FALSE)))
         Err ("Error: %s \n", szLIBERR);

      printf ("  Testing : %s ", pfd->szName);

      uRet = TestFile (pfd);
      switch (uRet)
         {
         case 0:  p = "ok";                            break;
         case 1:  p = "enexpected end of input file";  break;
         case 2:  p = "enexpected end of output file"; break;
         case 3:  p = "CRC error";                     break;
         default: p = "unknown error";                 break;
         }
      printf ("%s.\n", p);
      }       
   fclose (pld->fp);
   return 0;
   }



int AddLib  (PSZ pszLib, BOOL bMove)
   {
   FILEFINDBUF findbuf;
   HDIR        hdir;
   USHORT      i, uSearchCount, uAtts;
   USHORT      uRes, uParams, uFiles = 0;
   PLDESC      pldOut, pldIn;
   PSZ         psz1, psz2, pszParam, pszOutFile;
   PFDESC      pfd;
   char        szDesc [MAXDESCLEN+1];
   char        szDrive[_MAX_DRIVE], szDir[_MAX_DIR];

   pszOutFile = TEMPLIB;
   if (!(pldIn = OpenLib (pszLib)))
      {
      pszOutFile = pszLib;
      printf ("Creating Library file %s\n", pszLib);
      }
   else
      {
      printf ("Updating Library file %s\n", pszLib);
      for (i=0; i<pldIn->uCount; i++)
         {
         if (!(pfd = ReadFileInfo (pldIn, TRUE)))
            Err ("Error: %s \n", szLIBERR);

         pfd->uMode = LIB;
         AddToFileList (pfd);
         }
      }

   if (!(pldOut = MakeLibFile (pszOutFile, GetLibDesc (szDesc, (pldIn ? pldIn->pszDesc : NULL)))))
      Err ("Error: Unable to create library file %s", pszOutFile);

   uParams = ArgIs (NULL);

   uAtts = FILE_NORMAL | FILE_ARCHIVED |
           (bINCLSYSTEM ? FILE_SYSTEM : 0) |
           (bINCLHIDDEN ? FILE_HIDDEN : 0);

   for (i = (uParams==1 ? 0 : 1); i < uParams; i++)
      {
      if (!i) /*--- no params = all files when adding ---*/
         pszParam = "*.*";
      else
         pszParam = ArgGet (NULL, i);

      _splitpath (pszParam, szDrive, szDir, szDesc, szDesc);

      uSearchCount = 1, hdir = HDIR_SYSTEM;

      uRes = DosFindFirst(pszParam, &hdir, uAtts, &findbuf,
                          sizeof(findbuf), &uSearchCount, 0L);

      if (uRes)
         printf ("BAMSIB: no match found for: %s\n", pszParam);

      while(!uRes)
         {
         if (stricmp (findbuf.achName, pszLib) && stricmp (findbuf.achName, TEMPLIB))
            {
            pfd = malloc (sizeof (FDESC));

            pfd->uMethod = (bSTOREONLY ? STORE : HOSE);

            psz1 = ((psz2 = strrchr (findbuf.achName, ':'))  ? psz2+1 : findbuf.achName);
            psz1 = ((psz2 = strrchr (psz1, '\\')) ? psz2+1 : psz1);

            if ((psz1 = strchr (psz1, '.')) &&
                (!strnicmp (psz1, ".0", 2)   ||
                 !strnicmp (psz1, ".EBS", 4) ||
                 !strnicmp (psz1, ".BAMS", 4) ||
                 !strnicmp (psz1, ".ZIP", 4)))
               pfd->uMethod  = STORE;

            sprintf (pfd->szName, "%s%s%s", szDrive, szDir, findbuf.achName);
            FilGet4DosDesc (pfd->szName, pfd->szDesc);

            pfd->ulSize   = findbuf.cbFile;
            pfd->ulLen    = findbuf.cbFile;
            pfd->fDate    = findbuf.fdateLastWrite;
            pfd->fTime    = findbuf.ftimeLastWrite;
            pfd->uAtt     = findbuf.attrFile;
            pfd->uMode    = CMDLINE;

            AddToFileList (pfd);
            uFiles++;
            }
         uRes = DosFindNext(hdir, &findbuf, sizeof(findbuf), &uSearchCount);
         }
      DosFindClose (hdir);
      }

   WriteLibFromList (pldOut);
   fclose (pldOut->fp);
   if (pldIn)
      {
      fclose (pldIn->fp);
      unlink (pszLib);
      }
   if (pszOutFile != pszLib);
   rename (pszOutFile, pszLib);

   if (bMove)
      DeleteFilesInList ();

   return 0;
   }



int DelLib  (PSZ pszLib)
   {
   PFDESC fTmp, pfd;
   PLDESC pldOut, pldIn;
   USHORT i, uParams, uDeleteCount = 0;
   char   szDesc [MAXDESCLEN +1];

   if ((uParams = ArgIs (NULL)) < 2)
      Usage (1);

   if (!(pldIn = OpenLib (pszLib)))
      Err ("Error: %s \n", szLIBERR);

   /*--- read in file descriptors ---*/
   for (i=0; i<pldIn->uCount; i++)
      {
      if (!(pfd = ReadFileInfo (pldIn, TRUE)))
         Err ("Error: %s \n", szLIBERR);

      pfd->uMode = LIB;
      AddToFileList (pfd);
      }

   /*--- match file names ---*/
   for (fTmp = fList; fTmp; fTmp = fTmp->Next)
      if (MatchesParams (fTmp->szName, FALSE) && fTmp->uMode != DELET)
         {
         uDeleteCount++;
         fTmp->uMode = DELET;
         }

   if (!uDeleteCount)
      {
      printf ("No matching files found.\n");
      return 0;
      }

   if (!(pldOut = MakeLibFile (TEMPLIB, GetLibDesc (szDesc, pldIn->pszDesc))))
      Err ("can't create temp file", "");

   WriteLibFromList (pldOut);
   fclose (pldOut->fp);
   fclose (pldIn->fp);
   unlink (pszLib);
   rename (TEMPLIB, pszLib);

   return 0;
   }



int XLib  (PSZ pszLib)
   {
   PFDESC pfd;
   PLDESC pldIn;
   USHORT j, uExtractCount = 0;
   BOOL   bWrote;

   if (!(pldIn = OpenLib (pszLib)))
      Err ("Error: %s \n", szLIBERR);

   /*--- read in file descriptors ---*/
   for (j=0; j<pldIn->uCount; j++)
      {
      if (!(pfd = ReadFileInfo (pldIn, FALSE)))
         Err ("Error: %s \n", szLIBERR);

      bWrote = FALSE;
      if (MatchesParams (pfd->szName, TRUE))
         bWrote = WriteToFile (pfd, FALSE);

      if (bWrote)
         uExtractCount++;
      else
         SkipFileData (pfd);
      }
   fclose (pldIn->fp);
   return 0;
   }


DescLib (PSZ pszLib)
   {
   char   szDesc [MAXDESCLEN+1];
   USHORT i;
   PLDESC pldOut, pldIn;
   PFDESC pfd;
   ULONG  ulFilePos;
  
   /*--- open input lib ---*/
   if (!(pldIn = OpenLib (pszLib)))
      Err ("Error: %s", szLIBERR);

   /*--- open output lib ---*/
   pldOut = MakeLibFile (TEMPLIB, GetLibDesc (szDesc, NULL));

   if (*szDesc)
      printf (" Adding Library Description...");
   else
      printf (" Removing Library Description...");


   for (i=0; i<pldIn->uCount; i++)
      {
      if (!(pfd = ReadFileInfo (pldIn, TRUE)))
         Err ("Error: %s \n", szLIBERR);

      pfd->uMode = LIB;
      ulFilePos  = ftell (pldOut->fp);

      WriteTheDamnFile (pldOut, pfd, ulFilePos);
      }
   WriteMark  (pldOut->fp);
   ulFilePos = ftell (pldOut->fp);
   fseek (pldOut->fp, FILELENOFFSET, SEEK_SET);
   FilWriteLong  (pldOut->fp, ulFilePos);
   FilWriteShort (pldOut->fp, i);
   fclose (pldOut->fp);
   fclose (pldIn->fp);
   unlink (pszLib);
   rename (TEMPLIB, pszLib);
   printf ("\n");
   return 0;
   }


   
/*******************************************************************/
/*                                                                 */
/*                                                                 */
/*                                                                 */
/*******************************************************************/


void Usage (USHORT i)
   {
   PSZ p;

   switch (i)
      {
      case 1: p = szUsage1; break;
      case 2: p = szUsage2; break;
      case 3: p = szUsage3; break;
      }
   printf (p, PROGVER, DATED, TIME);
   exit (0);
   }



int _cdecl main (int argc, char *argv[])
   {
   PSZ    p1, p2;
   USHORT uCompression;

   fprintf (stderr, "Copyright (c) 1995 by Info Tech. Inc.  All Rights Reserved.\n\n");

   ArgBuildBlk ("? *^help *^FullHelp ^Examples ^a- ^d- ^l- ^v- ^t- ^x-"
                " ^y- ^n- ^i? ^z- ^s- ^h- ^c% ^e- ^m-");

   if (ArgFillBlk (argv))
      {
      fprintf (stderr, "%s\n", ArgGetErr ());
      Usage (1);
      }

   if (ArgIs ("FullHelp"))
      Usage (2);
   
   if (ArgIs ("Examples"))
      Usage (3);
   
   if (ArgIs ("help") || ArgIs ("?") || !ArgIs(NULL))
      Usage (1);
   /*--- Get Library File Name ---*/
   strcpy (szLib, ArgGet(NULL,0));
   p2 = strrchr (szLib, '\\');
   p1 = (p2 ? p2+1 : szLib);
   if (!strchr (p1, '.'))
      strcat (p1, EXT);

   if (ArgIs ("y"))
      iOVERWRITE = 1;

   if (ArgIs ("n"))
      iOVERWRITE = -1;

   bDEBUGMODE  = ArgIs ("z");
   bINCLSYSTEM = ArgIs ("s");
   bINCLHIDDEN = ArgIs ("h");
   bSTOREONLY  = FALSE;
   bLIBDESC    = ArgIs ("i");
   bFILEDESC   = !access ("DESCRIPT.ION", 0);

   uCompression = 3;

   if (ArgIs ("c"))
      {
      p1 = ArgGet ("c", 0);
      bSTOREONLY  = (*p1 == '0');
      if (!(uCompression = atoi (p1)))
         uCompression = 3;
      uCompression = min (3, uCompression);
      }

   /*--- this inits the compression module's work buffer pszWorkBuff---*/
   pszWorkBuff = malloc (35256U);
   Cmp2Init (pszWorkBuff, uCompression, 1);

   if (ArgIs ("l"))
      return ListLib (szLib, FALSE);     /*--- List ---*/
   if (ArgIs ("v"))
      return ListLib (szLib, TRUE);      /*--- List ---*/
   else if (ArgIs ("t"))
      return TestLib (szLib);            /*--- Test ---*/
   else if (ArgIs ("a"))
      return AddLib (szLib, FALSE);      /*--- Add  ---*/
   else if (ArgIs ("m"))
      return AddLib (szLib, TRUE);       /*--- Add  ---*/
   else if (ArgIs ("d"))
      return DelLib (szLib);             /*--- Del  ---*/
   else if (ArgIs ("x") || ArgIs ("e"))
      return XLib (szLib);               /*--- Extract ---*/
   else if (ArgIs ("i"))
      return DescLib (szLib);            /*--- Describe ---*/
   else
      return ListLib (szLib, FALSE);     /*--- List ---*/

   return 0;
   }

