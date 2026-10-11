; The installer of audient-device-closer.
;
; The version is handed in by the build: ISCC /DADC_VERSION=1.0.0 installer.iss.  A build
; without a version is a local one, and it says so instead of inventing one.

#ifndef ADC_VERSION
#  define ADC_VERSION "dev"
#endif

#define ADC_NAME "Audient Device Closer"
#define ADC_EXE "audient-device-closer.exe"
#define ADC_PUBLISHER "oov"
#define ADC_URL "https://github.com/oov/audient-device-closer"
; the only place this program is installed in
#define ADC_DIR "{autopf}\AudientDeviceCloser"

; the log check box on the uninstall options page, in the wizard's own units
#define LOG_CHECK_HEIGHT 17
; the folder line under the check box, in the wizard's own units
#define LOG_PATH_INDENT 16
#define LOG_PATH_GAP 4
; the gap between the Uninstall and the Cancel buttons, in the wizard's own units
#define BUTTON_GAP 10

[Setup]
AppId={{7A2C4E61-9B35-4D0F-8A47-2C1E5B9D3F80}
AppName={#ADC_NAME}
AppVersion={#ADC_VERSION}
AppPublisher={#ADC_PUBLISHER}
AppPublisherURL={#ADC_URL}
AppSupportURL={#ADC_URL}
DefaultDirName={#ADC_DIR}
DisableDirPage=yes
DisableReadyPage=yes
DefaultGroupName={#ADC_NAME}
DisableProgramGroupPage=yes
AppMutex=AudientDeviceCloser.Mutex
OutputDir=build\Release\dist
OutputBaseFilename=audient-device-closer_{#ADC_VERSION}_setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern dynamic
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\{#ADC_EXE}
PrivilegesRequired=admin
ChangesEnvironment=no

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"; LicenseFile: "LICENSE"; InfoBeforeFile: "PRIVILEGES.txt"
Name: "ja"; MessagesFile: "compiler:Languages\Japanese.isl"; LicenseFile: "LICENSE"; InfoBeforeFile: "PRIVILEGES-jp.txt"

[Files]
Source: "build\Release\x86_64\bin\{#ADC_EXE}"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\Release\x86_64\bin\audient-device-closer.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "README-jp.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "PRIVILEGES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "PRIVILEGES-jp.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#ADC_NAME}"; Filename: "{app}\{#ADC_EXE}"
Name: "{group}\{cm:UninstallProgram,{#ADC_NAME}}"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\{#ADC_EXE}"; Description: "{cm:LaunchProgram,{#ADC_NAME}}"; Flags: nowait postinstall skipifsilent runascurrentuser

[CustomMessages]
DeleteLogItem=Remove the log folder
ja.DeleteLogItem=ログのフォルダを削除する
UninstallOptionsName=Uninstall Options
ja.UninstallOptionsName=アンインストールのオプション
UninstallOptionsDescription=Choose what to remove, then click Uninstall.
ja.UninstallOptionsDescription=削除する内容を選び、アンインストールをクリックしてください。
UninstallButton=Uninstall
ja.UninstallButton=アンインストール
FixedDirError=The installation folder cannot be changed. The program is installed in %1.
ja.FixedDirError=インストール先は変更できません。プログラムは %1 にインストールされます。

[Code]
const
  { the buffer GetTextFaceW fills in, in characters }
  FontFaceBufferLength = 64;

var
  RemoveLogWanted: Boolean;

type
  TFontFaceName = array[0..FontFaceBufferLength - 1] of Char;

function CreateFontW(nHeight, nWidth, nEscapement, nOrientation, fnWeight: Integer;
  fdwItalic, fdwUnderline, fdwStrikeOut, fdwCharSet, fdwOutputPrecision,
  fdwClipPrecision, fdwQuality, fdwPitchAndFamily: Integer;
  lpszFace: String): Integer;
external 'CreateFontW@gdi32.dll stdcall';
function SelectObject(hdc: Integer; hgdiobj: Integer): Integer;
external 'SelectObject@gdi32.dll stdcall';
function GetTextFaceW(hdc: Integer; nCount: Integer; var lpFaceName: TFontFaceName): Integer;
external 'GetTextFaceW@gdi32.dll stdcall';
function GetDC(hWnd: Integer): Integer;
external 'GetDC@user32.dll stdcall';
function ReleaseDC(hWnd: Integer; hdc: Integer): Integer;
external 'ReleaseDC@user32.dll stdcall';
function DeleteObject(hObject: Integer): Integer;
external 'DeleteObject@gdi32.dll stdcall';

{ Asks GDI which font face it actually picked for the requested face name.
  A face that exists comes back unchanged; a missing one makes GDI silently
  substitute another font, so the names differ.  A static array is passed for
  the buffer: Pascal Script hands a var parameter to the DLL as the address
  of the variable itself, and only an inline value is safe for the API to
  write into. }
function FontFaceOf(const Requested: String): String;
var
  dc, hFont, old, len, I: Integer;
  buf: TFontFaceName;
begin
  Result := '';
  dc := GetDC(0);
  if dc = 0 then
    Exit;
  hFont := CreateFontW(0, 0, 0, 0, 400, 0, 0, 0, 1, 0, 0, 0, 0, Requested);
  if hFont <> 0 then
  begin
    old := SelectObject(dc, hFont);
    if old <> 0 then
    begin
      len := GetTextFaceW(dc, FontFaceBufferLength, buf);
      if len > 0 then
        for I := 1 to len do
          if buf[I - 1] <> #0 then
            Result := Result + buf[I - 1];
      SelectObject(dc, old);
    end;
    DeleteObject(hFont);
  end;
  ReleaseDC(0, dc);
end;

function FontExists(const FaceName: String): Boolean;
begin
  Result := FontFaceOf(FaceName) = FaceName;
end;

{ The Japanese translations read best in a Japanese font.  Segoe UI has no
  Japanese glyphs, so the system font-links them and the text ends up mixed.
  Switch the form to the best Japanese font installed, in order of
  preference; without any of them, keep the default.  The layout itself was
  already laid out for the default font, so the form is switched as early as
  possible and every caption is given room to spare.

  Changing the form font recreates the window handles.  The license box and
  its siblings get their text color from a hook that runs while the text is
  loaded, which happened when the form was constructed, so in dark mode the
  text would be left dark on dark.  Reloading the text runs the hook again
  and restores the styled color. }
procedure ReloadRichText(Memo: TRichEditViewer);
begin
  Memo.RTFText := Memo.Lines.Text;
end;

procedure ApplyJapaneseFont(Form: TForm);
begin
  if ActiveLanguage <> 'ja' then
    Exit;
  if FontExists('Yu Gothic UI') then
    Form.Font.Name := 'Yu Gothic UI'
  else if FontExists('Meiryo UI') then
    Form.Font.Name := 'Meiryo UI'
  else if FontExists('MS UI Gothic') then
    Form.Font.Name := 'MS UI Gothic';
end;

procedure ApplyJapaneseFontToWizard();
begin
  ApplyJapaneseFont(WizardForm);
  ReloadRichText(WizardForm.LicenseMemo);
  ReloadRichText(WizardForm.InfoBeforeMemo);
  ReloadRichText(WizardForm.InfoAfterMemo);
end;

{ The built-in uninstall confirmation and the completed message are
  displayed unconditionally in a normal run and cannot be turned off from
  the script; both duplicate what the options page already does.  A normal
  run therefore hands the work over to a silent copy of itself and exits:
  the silent run skips both dialogs, and the marker switch makes it show
  the options page instead.  A plain silent run keeps its standard
  behavior, and if the copy cannot be started the normal flow runs as
  before. }
{ A normal uninstaller run shows two dialogs the script cannot turn off,
  and both are redundant here: the confirmation asks the same question as
  the options page, and the completed message is noise after it.  The
  dialogs are hard-coded to appear unless the uninstaller runs silently,
  and silence cannot be asked for from the script, so a normal run starts
  a silent copy of itself with a marker switch and steps aside.  The copy
  skips both dialogs, and the marker makes its options page appear in
  place of the confirmation.  A plain silent run keeps its standard
  behavior, and if the copy cannot be started the normal flow runs as
  before, dialogs included. }
function InitializeUninstall(): Boolean;
var
  ResultCode: Integer;
begin
  Result := True;
  if not UninstallSilent then
    if Exec(ExpandConstant('{uninstallexe}'), '/SILENT /ADCOptions', '',
        SW_SHOW, ewNoWait, ResultCode) then
      Result := False;
end;

function InitializeSetup(): Boolean;
begin
  { The install location is fixed: there is no directory page, and the
    /DIR switch is refused rather than quietly installing somewhere else. }
  if ExpandConstant('{param:dir|}') <> '' then
  begin
    MsgBox(FmtMessage(CustomMessage('FixedDirError'), [ExpandConstant('{#ADC_DIR}')]), mbError, MB_OK);
    Result := False;
    Exit;
  end;
  Result := True;
end;

procedure InitializeWizard();
begin
  ApplyJapaneseFontToWizard();
end;

procedure InitializeUninstallProgressForm();
var
  UninstallPage: TNewNotebookPage;
  UninstallButton: TNewButton;
  DeleteLogCheck: TNewCheckBox;
  DeleteLogPath: TNewStaticText;
  OriginalPageNameLabel: string;
  OriginalPageDescriptionLabel: string;
  OriginalCancelButtonEnabled: Boolean;
  OriginalCancelButtonModalResult: Integer;
begin
  RemoveLogWanted := True;
  ApplyJapaneseFont(UninstallProgressForm);
  { A silent run shows the page only when the normal-mode run above handed
    the work over.  The marker switch is looked up in the command tail
    because a valueless switch cannot be told apart from an absent one with
    the param constant. }
  if UninstallSilent and (Pos('/ADCOPTIONS', AnsiUppercase(GetCmdTail)) = 0) then
    Exit;

  { A page of our own in place of the progress, until the user has chosen.
    The uninstall does not start until the form is shown again below, so the
    check box can be answered at leisure. }
  UninstallPage := TNewNotebookPage.Create(UninstallProgressForm);
  UninstallPage.Notebook := UninstallProgressForm.InnerNotebook;
  UninstallPage.Parent := UninstallProgressForm.InnerNotebook;
  UninstallPage.Align := alClient;

  { What the page asks: whether the log folder is removed as well.
    The check box is given the whole width of the page content: a
    caption has to fit whatever language it is translated into, and
    a check box is drawn on its left side only. }
  DeleteLogCheck := TNewCheckBox.Create(UninstallProgressForm);
  DeleteLogCheck.Parent := UninstallPage;
  DeleteLogCheck.Left := UninstallProgressForm.StatusLabel.Left;
  DeleteLogCheck.Top := UninstallProgressForm.StatusLabel.Top;
  DeleteLogCheck.Width := UninstallProgressForm.StatusLabel.Width;
  DeleteLogCheck.Height := ScaleY({#LOG_CHECK_HEIGHT});
  DeleteLogCheck.Caption := CustomMessage('DeleteLogItem');
  DeleteLogCheck.Checked := True;

  { Where the log folder is.  It is its own line so that it wraps instead of
    pushing the check box off the page. }
  DeleteLogPath := TNewStaticText.Create(UninstallProgressForm);
  DeleteLogPath.Parent := UninstallPage;
  DeleteLogPath.Left := DeleteLogCheck.Left + ScaleX({#LOG_PATH_INDENT});
  DeleteLogPath.Top := DeleteLogCheck.Top + DeleteLogCheck.Height + ScaleY({#LOG_PATH_GAP});
  DeleteLogPath.Width := UninstallProgressForm.StatusLabel.Width;
  DeleteLogPath.AutoSize := False;
  DeleteLogPath.WordWrap := True;
  DeleteLogPath.ShowAccelChar := False;
  DeleteLogPath.Caption := ExpandConstant('{commonappdata}\AudientDeviceCloser');
  DeleteLogPath.AdjustHeight;

  { The button that starts the uninstall.  The Cancel button of the form
    cancels this page: it closes the loop below with mrCancel. }
  UninstallButton := TNewButton.Create(UninstallProgressForm);
  UninstallButton.Parent := UninstallProgressForm;
  UninstallButton.Caption := CustomMessage('UninstallButton');
  UninstallButton.Width := UninstallProgressForm.CalculateButtonWidth([UninstallButton.Caption]);
  UninstallButton.Height := UninstallProgressForm.CancelButton.Height;
  UninstallButton.Left := UninstallProgressForm.CancelButton.Left - UninstallButton.Width - ScaleX({#BUTTON_GAP});
  UninstallButton.Top := UninstallProgressForm.CancelButton.Top;
  UninstallButton.TabOrder := UninstallProgressForm.CancelButton.TabOrder;
  UninstallButton.Default := True;
  UninstallButton.ModalResult := mrOk;

  { The page takes over the title and the description of the progress, and the
    Cancel button, which the progress keeps disabled, becomes the cancel of
    this page. }
  OriginalPageNameLabel := UninstallProgressForm.PageNameLabel.Caption;
  OriginalPageDescriptionLabel := UninstallProgressForm.PageDescriptionLabel.Caption;
  OriginalCancelButtonEnabled := UninstallProgressForm.CancelButton.Enabled;
  OriginalCancelButtonModalResult := UninstallProgressForm.CancelButton.ModalResult;
  UninstallProgressForm.PageNameLabel.Caption := CustomMessage('UninstallOptionsName');
  UninstallProgressForm.PageDescriptionLabel.Caption := CustomMessage('UninstallOptionsDescription');
  UninstallProgressForm.CancelButton.Enabled := True;
  UninstallProgressForm.CancelButton.ModalResult := mrCancel;

  UninstallProgressForm.InnerNotebook.ActivePage := UninstallPage;

  { Stay on the page until the user has chosen. }
  if UninstallProgressForm.ShowModal = mrCancel then
    Abort;
  RemoveLogWanted := DeleteLogCheck.Checked;

  { Back to the page the uninstall runs on. }
  UninstallButton.Visible := False;
  UninstallProgressForm.PageNameLabel.Caption := OriginalPageNameLabel;
  UninstallProgressForm.PageDescriptionLabel.Caption := OriginalPageDescriptionLabel;
  UninstallProgressForm.CancelButton.Enabled := OriginalCancelButtonEnabled;
  UninstallProgressForm.CancelButton.ModalResult := OriginalCancelButtonModalResult;
  UninstallProgressForm.InnerNotebook.ActivePage := UninstallProgressForm.InstallingPage;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
  begin
    if RemoveLogWanted then
      DelTree(ExpandConstant('{commonappdata}\AudientDeviceCloser'), True, True, True);
  end;
end;

[UninstallRun]
Filename: "{sys}\schtasks.exe"; Parameters: "/Delete /TN AudientDeviceCloser\Device recovery after resume from sleep /F"; Flags: runhidden; RunOnceId: "RemoveTask"
