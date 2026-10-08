// ExtractGUI.cpp

#include "StdAfx.h"

#include "../../../Common/IntToString.h"
#include "../../../Common/StringConvert.h"

#include "../../../Windows/FileDir.h"
#include "../../../Windows/FileFind.h"
#include "../../../Windows/FileName.h"
#include "../../../Windows/Thread.h"

#include "../FileManager/ExtractCallback.h"
#include "../FileManager/FormatUtils.h"
#include "../FileManager/LangUtils.h"
#include "../FileManager/resourceGui.h"
#include "../FileManager/OverwriteDialogRes.h"

#include "../Common/ArchiveExtractCallback.h"
#include "../Common/PropIDUtils.h"

#include "../Explorer/MyMessages.h"

#include "resource2.h"
#include "ExtractRes.h"

#include "ExtractDialog.h"
#include "ExtractGUI.h"
#include "HashGUI.h"

#include "../FileManager/PropertyNameRes.h"

using namespace NWindows;
using namespace NFile;
using namespace NDir;

static const wchar_t * const kIncorrectOutDir = L"Incorrect output directory path";

#ifndef Z7_SFX

static void AddValuePair(UString &s, UINT resourceID, UInt64 value, bool addColon = true)
{
  AddLangString(s, resourceID);
  if (addColon)
    s.Add_Colon();
  s.Add_Space();
  s.Add_UInt64(value);
  s.Add_LF();
}

static void AddSizePair(UString &s, UINT resourceID, UInt64 value)
{
  AddLangString(s, resourceID);
  s += ": ";
  AddSizeValue(s, value);
  s.Add_LF();
}

#endif

class CThreadExtracting: public CProgressThreadVirt
{
  HRESULT ProcessVirt() Z7_override;
public:
  /*
  #ifdef Z7_EXTERNAL_CODECS
  const CExternalCodecs *externalCodecs;
  #endif
  */

  CCodecs *codecs;
  CExtractCallbackImp *ExtractCallbackSpec;
  const CObjectVector<COpenType> *FormatIndices;
  const CIntVector *ExcludedFormatIndices;

  UStringVector *ArchivePaths;
  UStringVector *ArchivePathsFull;
  const NWildcard::CCensorNode *WildcardCensor;
  const CExtractOptions *Options;

  #ifndef Z7_SFX
  CHashBundle *HashBundle;
  virtual void ProcessWasFinished_GuiVirt() Z7_override;
  #endif

  CMyComPtr<IFolderArchiveExtractCallback> FolderArchiveExtractCallback;
  UString Title;

  CPropNameValPairs Pairs;

#ifndef Z7_SFX
  FString FirstExtractedPath;
#endif
};


#ifndef Z7_SFX
void CThreadExtracting::ProcessWasFinished_GuiVirt()
{
  if (HashBundle && !Pairs.IsEmpty())
    ShowHashResults(Pairs, *this);
}
#endif

HRESULT CThreadExtracting::ProcessVirt()
{
  CDecompressStat Stat;
  
  #ifndef Z7_SFX
  /*
  if (HashBundle)
    HashBundle->Init();
  */
  #endif

  HRESULT res = Extract(
      /*
      #ifdef Z7_EXTERNAL_CODECS
      externalCodecs,
      #endif
      */
      codecs,
      *FormatIndices, *ExcludedFormatIndices,
      *ArchivePaths, *ArchivePathsFull,
      *WildcardCensor, *Options,
      ExtractCallbackSpec, ExtractCallbackSpec, FolderArchiveExtractCallback,
      #ifndef Z7_SFX
        HashBundle,
      #endif
      FinalMessage.ErrorMessage.Message, Stat);
  
  #ifndef Z7_SFX
  if (res == S_OK && ExtractCallbackSpec->IsOK())
  {
    FirstExtractedPath = Stat.FirstExtractedPath;
    if (HashBundle)
    {
      AddValuePair(Pairs, IDS_ARCHIVES_COLON, Stat.NumArchives);
      AddSizeValuePair(Pairs, IDS_PROP_PACKED_SIZE, Stat.PackSize);
      AddHashBundleRes(Pairs, *HashBundle);
    }
    else if (Options->TestMode)
    {
      UString s;
    
      AddValuePair(s, IDS_ARCHIVES_COLON, Stat.NumArchives, false);
      AddSizePair(s, IDS_PROP_PACKED_SIZE, Stat.PackSize);

      if (Stat.NumFolders != 0)
        AddValuePair(s, IDS_PROP_FOLDERS, Stat.NumFolders);
      AddValuePair(s, IDS_PROP_FILES, Stat.NumFiles);
      AddSizePair(s, IDS_PROP_SIZE, Stat.UnpackSize);
      if (Stat.NumAltStreams != 0)
      {
        s.Add_LF();
        AddValuePair(s, IDS_PROP_NUM_ALT_STREAMS, Stat.NumAltStreams);
        AddSizePair(s, IDS_PROP_ALT_STREAMS_SIZE, Stat.AltStreams_UnpackSize);
      }
      s.Add_LF();
      AddLangString(s, IDS_MESSAGE_NO_ERRORS);
      FinalMessage.OkMessage.Title = Title;
      FinalMessage.OkMessage.Message = s;
    }
  }
  #endif

  return res;
}

#ifndef Z7_SFX
#include <shlobj_core.h>
#include "../../../Common/ListFileUtils.h"
#include "../../../Windows/DLL.h"
#include "../../../Windows/ErrorMsg.h"
#include "../FileManager/ComboDialog.h"
static void BrowseToPath(
    bool explore,
    UString &path)
{
  if (explore /* || (GetFileAttributes(path.Ptr()) & FILE_ATTRIBUTE_DIRECTORY)*/) {
    ShellExecute(NULL, L"explore", path.Ptr(), NULL, NULL, SW_SHOW);
  } else {
  #if (NTDDI_VERSION >= NTDDI_WINXP)
    LPITEMIDLIST pidl = ILCreateFromPath(path.Ptr());
    if (pidl) {
      SHOpenFolderAndSelectItems(pidl,0,0,0);
      ILFree(pidl);
    }
  #else
    UString args = L"/n,/select,\"" + path + L"\"";
    ShellExecute(NULL, L"open", L"explorer.exe", args.Ptr(), NULL, SW_SHOW);
  #endif
  }
}

// ---------- extract triggers ----------
// After a successful extraction, the rules read from Triggers.txt can ask for
// a text string and run an external command line. See DOC/Extract-Triggers.md.

static const wchar_t * const kTriggersFileName = L"Triggers.txt";

struct CExtractTrigger
{
  UString Dest;    // glob pattern matched against the destination folder
  UString Title;   // dialog caption
  UString Ask;     // dialog label; empty = don't show the dialog
  UString Default; // initial text of the dialog
  UString Cmd;     // command line template

  bool IsDefined() const { return !Dest.IsEmpty() && !Cmd.IsEmpty(); }
};

// NExplorer::ShowErrorMessage() is muted when 7zG runs with "-y", which is how
// 7zFM calls it, so trigger errors would never be seen. Always show them here.
static void ShowTriggerError(HWND hwnd, const UString &text)
{
  ::MessageBoxW(hwnd, text, L"7-Zip-Zstandard", MB_OK | MB_ICONSTOP);
}

static bool IsPathSep(wchar_t c) { return c == L'\\' || c == L'/'; }

static void TrimPathTail(UString &s)
{
  while (!s.IsEmpty() && IsPathSep(s.Back()))
    s.DeleteBack();
}

static wchar_t ToLowerAscii(wchar_t c)
{
  if (c >= L'A' && c <= L'Z')
    return (wchar_t)(c - L'A' + L'a');
  return c;
}

static bool HasNoPathSep(const wchar_t *s)
{
  for (; *s; s++)
    if (IsPathSep(*s))
      return false;
  return true;
}

// '*' and '?' don't match a path separator, "**" matches anything.
static bool MatchGlob(const wchar_t *p, const wchar_t *s)
{
  for (;;)
  {
    const wchar_t pc = *p;

    if (pc == L'*')
    {
      unsigned numStars = 0;
      while (*p == L'*') { p++; numStars++; }
      const bool crossSep = (numStars >= 2);
      if (crossSep && IsPathSep(*p))
        p++;
      if (*p == 0)
        return crossSep || HasNoPathSep(s);
      for (;;)
      {
        if (MatchGlob(p, s))
          return true;
        if (*s == 0 || (!crossSep && IsPathSep(*s)))
          return false;
        s++;
      }
    }

    if (pc == 0)
      return *s == 0;
    if (*s == 0)
      return false;
    if (pc == L'?')
    {
      if (IsPathSep(*s))
        return false;
    }
    else if (ToLowerAscii(*s) != ToLowerAscii(pc))
    {
      // '\\' and '/' are the same separator here:
      if (!IsPathSep(pc) || !IsPathSep(*s))
        return false;
    }
    p++;
    s++;
  }
}

static bool MatchDest(const UString &pattern, const UString &dest)
{
  UString p = pattern;
  TrimPathTail(p);
  UString d = dest;
  TrimPathTail(d);

  if (MatchGlob(p.Ptr(), d.Ptr()))
    return true;

  // 'D:\a\*' and 'D:\a\**' must also match the folder 'D:\a' itself:
  int sepPos = -1;
  for (int i = (int)p.Len() - 1; i >= 0; i--)
    if (IsPathSep(p[i])) { sepPos = i; break; }
  if (sepPos < 0)
    return false;

  const unsigned restStart = (unsigned)sepPos + 1;
  for (unsigned i = restStart; i < p.Len(); i++)
    if (p[i] != L'*')
      return false;
  if (restStart == p.Len())
    return false;

  return MatchGlob(p.Left((unsigned)sepPos).Ptr(), d.Ptr());
}

static bool GetTriggersFilePath(FString &path)
{
  path = NDLL::GetModuleDirPrefix();
  path += kTriggersFileName;
  if (NFind::DoesFileExist_Raw(path))
    return true;

  wchar_t buf[MAX_PATH * 4];
  const DWORD len = ::GetEnvironmentVariableW(L"APPDATA", buf, (DWORD)(Z7_ARRAY_SIZE(buf) - 1));
  if (len == 0 || len >= Z7_ARRAY_SIZE(buf) - 1)
    return false;
  UString s = buf;
  s += L"\\7-Zip-Zstandard\\";
  s += kTriggersFileName;
  path = us2fs(s);
  return NFind::DoesFileExist_Raw(path);
}

static bool LoadTriggers(CObjectVector<CExtractTrigger> &triggers)
{
  FString path;
  if (!GetTriggersFilePath(path))
    return false;

  // UTF-8 (with or without BOM), UTF-16 LE, then the system code page
  static const UINT kCodePages[] = { CP_UTF8, Z7_WIN_CP_UTF16, CP_ACP };

  UStringVector lines;
  DWORD lastError = 0;
  bool parsed = false;
  for (unsigned cp = 0; cp < Z7_ARRAY_SIZE(kCodePages) && !parsed; cp++)
  {
    lines.Clear();
    lastError = 0;
    parsed = ReadNamesFromListFile2(path, lines, kCodePages[cp], lastError);
  }

  if (!parsed)
  {
    UString s = L"Cannot read ";
    s += fs2us(path);
    s.Add_LF();
    s.Add_LF();
    s += NError::MyFormatMessage(lastError);
    ShowTriggerError(NULL, s);
    return false;
  }

  CExtractTrigger cur;
  bool inBlock = false;

  FOR_VECTOR (i, lines)
  {
    const UString &line = lines[i];
    if (line.IsEmpty())
      continue;
    const wchar_t c0 = line[0];
    if (c0 == L'#' || c0 == L';')
      continue;

    UString key, value;
    const int eqPos = line.Find(L'=');
    if (eqPos < 0)
      key = line;
    else
    {
      key = line.Left((unsigned)eqPos);
      value = line.Mid((unsigned)eqPos + 1, line.Len() - (unsigned)eqPos - 1);
    }
    key.Trim();
    value.Trim();
    if (key.IsEmpty())
      continue;
    if (key[0] == L'[' && key.Back() == L']')
    {
      key.Delete(0);
      key.DeleteBack();
    }

    if (key.IsEqualTo_NoCase(L"trigger"))
    {
      if (inBlock && cur.IsDefined())
        triggers.Add(cur);
      cur = CExtractTrigger();
      inBlock = true;
      continue;
    }
    if (key.IsEqualTo_NoCase(L"end"))
    {
      if (inBlock && cur.IsDefined())
        triggers.Add(cur);
      cur = CExtractTrigger();
      inBlock = false;
      continue;
    }
    if (!inBlock)
      continue;

    if (key.IsEqualTo_NoCase(L"dest"))
      cur.Dest = value;
    else if (key.IsEqualTo_NoCase(L"cmd"))
      cur.Cmd = value;
    else if (key.IsEqualTo_NoCase(L"ask"))
      cur.Ask = value;
    else if (key.IsEqualTo_NoCase(L"title"))
      cur.Title = value;
    else if (key.IsEqualTo_NoCase(L"default"))
      cur.Default = value;
  }

  if (inBlock && cur.IsDefined())
    triggers.Add(cur);
  return !triggers.IsEmpty();
}

// %text%, %dest% (%path%), %arc%, %aname%; '%%' is a literal '%'
static void ExpandCommand(const UString &tmpl, const UString &text,
    const UString &dest, const UString &arc, const UString &arcName, UString &destCmd)
{
  destCmd.Empty();
  const unsigned len = tmpl.Len();
  unsigned i = 0;

  while (i < len)
  {
    if (tmpl[i] != L'%')
    {
      destCmd += tmpl[i];
      i++;
      continue;
    }

    const int closePos = tmpl.Find(L'%', i + 1);
    if (closePos < 0)
    {
      destCmd += L'%';
      i++;
      continue;
    }

    const UString name = tmpl.Mid(i + 1, (unsigned)closePos - i - 1);
    if (name.IsEmpty())
    {
      destCmd += L'%';
      i = (unsigned)closePos + 1;
      continue;
    }

    const UString *val = NULL;
    if (name.IsEqualTo_NoCase(L"text"))
      val = &text;
    else if (name.IsEqualTo_NoCase(L"dest") || name.IsEqualTo_NoCase(L"path"))
      val = &dest;
    else if (name.IsEqualTo_NoCase(L"arc") || name.IsEqualTo_NoCase(L"archive"))
      val = &arc;
    else if (name.IsEqualTo_NoCase(L"aname"))
      val = &arcName;

    if (!val)
    {
      destCmd += L'%';
      i++;
      continue;
    }
    destCmd += *val;
    i = (unsigned)closePos + 1;
  }
}

static bool RunTriggerCommand(HWND hwnd, const UString &cmd, const UString &workDir, DWORD &exitCode)
{
  STARTUPINFOW si;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);

  PROCESS_INFORMATION pi;
  if (!::CreateProcessW(NULL, cmd.Ptr_non_const(), NULL, NULL, FALSE, 0, NULL,
      workDir.IsEmpty() ? NULL : workDir.Ptr(), &si, &pi))
  {
    UString s = L"Cannot run:";
    s.Add_LF();
    s += cmd;
    s.Add_LF();
    s.Add_LF();
    s += NError::MyFormatMessage(::GetLastError());
    ShowTriggerError(hwnd, s);
    return false;
  }

  ::CloseHandle(pi.hThread);
  ::WaitForSingleObject(pi.hProcess, INFINITE);
  exitCode = (DWORD)(Int32)-1;
  ::GetExitCodeProcess(pi.hProcess, &exitCode);
  ::CloseHandle(pi.hProcess);
  return true;
}

static void RunExtractTriggers(HWND hwnd, const FString &outputDir, const UStringVector &archivePathsFull)
{
  CObjectVector<CExtractTrigger> triggers;
  if (!LoadTriggers(triggers))
    return;

  UString dest = fs2us(outputDir);
  TrimPathTail(dest);

  UString arc, arcName;
  if (!archivePathsFull.IsEmpty())
  {
    arc = archivePathsFull[0];
    arcName = arc;
    const int sep = arcName.ReverseFind_PathSepar();
    if (sep >= 0)
      arcName.DeleteFrontal((unsigned)sep + 1);
    const int dot = arcName.ReverseFind_Dot();
    if (dot > 0)
      arcName.DeleteFrom((unsigned)dot);
  }

  FOR_VECTOR (ti, triggers)
  {
    const CExtractTrigger &tr = triggers[ti];
    if (!MatchDest(tr.Dest, dest))
      continue;

    UString text = tr.Default;
    if (!tr.Ask.IsEmpty())
    {
      CComboDialog dialog;
      dialog.Title = tr.Title.IsEmpty() ? UString(L"7-Zip") : tr.Title;
      dialog.Static = tr.Ask;
      dialog.Value = text;
      if (dialog.Create(hwnd) != IDOK)
        return;
      text = dialog.Value;
    }

    UString cmd;
    ExpandCommand(tr.Cmd, text, dest, arc, arcName, cmd);
    if (cmd.IsEmpty())
      continue;

    DWORD exitCode = 0;
    if (!RunTriggerCommand(hwnd, cmd, dest, exitCode))
      return;
    if (exitCode != 0)
    {
      UString s = L"The trigger command exited with code ";
      s.Add_UInt32(exitCode);
      s.Add_LF();
      s.Add_LF();
      s += cmd;
      ShowTriggerError(hwnd, s);
      return;
    }
  }
}
#endif

HRESULT ExtractGUI(
    // DECL_EXTERNAL_CODECS_LOC_VARS
    CCodecs *codecs,
    const CObjectVector<COpenType> &formatIndices,
    const CIntVector &excludedFormatIndices,
    UStringVector &archivePaths,
    UStringVector &archivePathsFull,
    const NWildcard::CCensorNode &wildcardCensor,
    CExtractOptions &options,
    #ifndef Z7_SFX
    CHashBundle *hb,
    #endif
    bool showDialog,
    bool &messageWasDisplayed,
    CExtractCallbackImp *extractCallback,
    HWND hwndParent)
{
  messageWasDisplayed = false;

  CThreadExtracting extracter;
  /*
  #ifdef Z7_EXTERNAL_CODECS
  extracter.externalCodecs = _externalCodecs;
  #endif
  */
  extracter.codecs = codecs;
  extracter.FormatIndices = &formatIndices;
  extracter.ExcludedFormatIndices = &excludedFormatIndices;

#ifndef Z7_SFX
  bool OpnTrgFold = false;
#endif
  if (!options.TestMode)
  {
    FString outputDir = options.OutputDir;
    #ifndef UNDER_CE
    if (outputDir.IsEmpty())
      GetCurrentDir(outputDir);
    #endif
    if (showDialog)
    {
      CExtractDialog dialog;
      FString outputDirFull;
      if (!MyGetFullPathName(outputDir, outputDirFull))
      {
        ShowErrorMessage(kIncorrectOutDir);
        messageWasDisplayed = true;
        return E_FAIL;
      }
      NName::NormalizeDirPathPrefix(outputDirFull);

      dialog.DirPath = fs2us(outputDirFull);

      dialog.OverwriteMode = options.OverwriteMode;
      dialog.OverwriteMode_Force = options.OverwriteMode_Force;
      dialog.PathMode = options.PathMode;
      dialog.PathMode_Force = options.PathMode_Force;
      dialog.ElimDup = options.ElimDup;

      if (archivePathsFull.Size() == 1)
        dialog.ArcPath = archivePathsFull[0];

      #ifndef Z7_SFX
      // dialog.AltStreams = options.NtOptions.AltStreams;
      dialog.NtSecurity = options.NtOptions.NtSecurity;
      if (extractCallback->PasswordIsDefined)
        dialog.Password = extractCallback->Password;
      #endif

      if (dialog.Create(hwndParent) != IDOK)
        return E_ABORT;

      outputDir = us2fs(dialog.DirPath);

      options.OverwriteMode = dialog.OverwriteMode;
      options.PathMode = dialog.PathMode;
      options.ElimDup = dialog.ElimDup;
      
      #ifndef Z7_SFX
      OpnTrgFold = dialog.OpnTrgFold.Val;
      // options.NtOptions.AltStreams = dialog.AltStreams;
      options.NtOptions.NtSecurity = dialog.NtSecurity;
      extractCallback->Password = dialog.Password;
      extractCallback->PasswordIsDefined = !dialog.Password.IsEmpty();
      #endif
    }
    #ifndef Z7_SFX
    else if (!options.OutputDir.IsEmpty()) // don't open target folder if extract here
    {
      // load setting "open target folder" from registry saved by previous dialog
      NExtract::CInfo _info;
      _info.Load();
      OpnTrgFold = _info.OpnTrgFold.Val;
    }
    #endif
    if (!MyGetFullPathName(outputDir, options.OutputDir))
    {
      ShowErrorMessage(kIncorrectOutDir);
      messageWasDisplayed = true;
      return E_FAIL;
    }
    NName::NormalizeDirPathPrefix(options.OutputDir);
    
    /*
    if (!CreateComplexDirectory(options.OutputDir))
    {
      UString s = GetUnicodeString(NError::MyFormatMessage(GetLastError()));
      UString s2 = MyFormatNew(IDS_CANNOT_CREATE_FOLDER,
      #ifdef Z7_LANG
      0x02000603,
      #endif
      options.OutputDir);
      s2.Add_LF();
      s2 += s;
      MyMessageBox(s2);
      return E_FAIL;
    }
    */
  }
  
  UString title = LangString(options.TestMode ? IDS_PROGRESS_TESTING : IDS_PROGRESS_EXTRACTING);

  extracter.Title = title;
  extracter.ExtractCallbackSpec = extractCallback;
  extracter.ExtractCallbackSpec->ProgressDialog = &extracter;
  extracter.FolderArchiveExtractCallback = extractCallback;
  extracter.ExtractCallbackSpec->Init();

  extracter.CompressingMode = false;

  extracter.ArchivePaths = &archivePaths;
  extracter.ArchivePathsFull = &archivePathsFull;
  extracter.WildcardCensor = &wildcardCensor;
  extracter.Options = &options;
  #ifndef Z7_SFX
  extracter.HashBundle = hb;
  #endif

  extracter.IconID = IDI_ICON;

  RINOK(extracter.Create(title, hwndParent))
  messageWasDisplayed = extracter.ThreadFinishedOK && extracter.MessagesDisplayed;

#ifndef Z7_SFX
  // user-defined triggers:
  if (!options.TestMode && extracter.Result == S_OK && extracter.ExtractCallbackSpec->IsOK())
    RunExtractTriggers(hwndParent, options.OutputDir, archivePathsFull);

  // browse/navigate to target path:
  if (OpnTrgFold && extracter.Result == S_OK) {
    // obtain path (directory or file) from first extracted:
    UString extrPath = extracter.FirstExtractedPath;
    if (extrPath.IsEmpty()) {
      extrPath = options.OutputDir;
    }
    else
    if (!options.OutputDir.IsEmpty()) {
      // first subpath relative selected in dialog or given by options.OutputDir:
      UString outDir = options.OutputDir;
      if (outDir.Back() != WCHAR_PATH_SEPARATOR)
        outDir += WCHAR_PATH_SEPARATOR;
      extrPath = extracter.FirstExtractedPath;
      if (extrPath.IsPrefixedBy(outDir)) {
        int subIdx = extrPath.Find(WCHAR_PATH_SEPARATOR, outDir.Len());
        if (subIdx != -1) {
          extrPath = extrPath.Left(subIdx-1);
        }
      }
    }
    if (!extrPath.IsEmpty()) {
      BrowseToPath(0 /* showDialog */, extrPath);
    }
  }
#endif
  return extracter.Result;
}
