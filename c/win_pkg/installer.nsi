; 무인매장 키오스크 트래킹 — 윈도우 설치형 패키지 (NSIS)
; 빌드: makensis -DDISTDIR=<패키지 폴더> -DOUTFILE=<출력 exe> installer.nsi
; - 사용자 폴더(%LocalAppData%)에 설치 → 관리자 권한 불필요, 데이터 쓰기 가능
; - 바탕화면/시작 메뉴 바로가기 + 제거 프로그램 등록
; - 제거 시 프로그램만 삭제하고 수집 데이터(data/·설정·로그)는 보존

Unicode true
!include "MUI2.nsh"

!ifndef DISTDIR
  !define DISTDIR "..\dist\kiosk_tracking"
!endif
!ifndef OUTFILE
  !define OUTFILE "kiosk_tracking_setup.exe"
!endif

Name "무인매장 키오스크 트래킹"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\KioskTracking"
RequestExecutionLevel user
SetCompressor /SOLID lzma

!define MUI_WELCOMEPAGE_TITLE "무인매장 키오스크 트래킹 설치"
!define MUI_WELCOMEPAGE_TEXT "엣지 비전 AI 매장 관제 파이프라인을 설치합니다.$\r$\n$\r$\n필요한 라이브러리가 전부 동봉되어 있어 별도 설치가 필요 없습니다.$\r$\n설치 후 바탕화면의 '매장 트래킹 시작' 아이콘을 실행하세요."
!define MUI_FINISHPAGE_RUN "$INSTDIR\START.bat"
!define MUI_FINISHPAGE_RUN_TEXT "지금 바로 실행 (점주 페이지가 브라우저로 열립니다)"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "Korean"

Section "본체"
  SetOutPath "$INSTDIR"
  ; 재설치 시 현장에서 조정한 설정 보존 (동봉 프리셋은 첫 설치에만 적용)
  IfFileExists "$INSTDIR\store_settings.json" 0 +2
    Rename "$INSTDIR\store_settings.json" "$INSTDIR\store_settings.keep"
  File /r "${DISTDIR}\*"
  IfFileExists "$INSTDIR\store_settings.keep" 0 +3
    Delete "$INSTDIR\store_settings.json"
    Rename "$INSTDIR\store_settings.keep" "$INSTDIR\store_settings.json"

  ; 바로가기
  CreateShortCut "$DESKTOP\매장 트래킹 시작.lnk" "$INSTDIR\START.bat" "" "" 0 SW_SHOWNORMAL "" "무인매장 트래킹 시작 (자동 재시작 포함)"
  CreateDirectory "$SMPROGRAMS\무인매장 트래킹"
  CreateShortCut "$SMPROGRAMS\무인매장 트래킹\매장 트래킹 시작.lnk" "$INSTDIR\START.bat"
  CreateShortCut "$SMPROGRAMS\무인매장 트래킹\점주 페이지 열기.lnk" "http://localhost:8765"
  CreateShortCut "$SMPROGRAMS\무인매장 트래킹\설치 안내서.lnk" "$INSTDIR\README_설치안내.md"
  CreateShortCut "$SMPROGRAMS\무인매장 트래킹\제거.lnk" "$INSTDIR\uninstall.exe"

  ; 제거 프로그램 등록 (현재 사용자)
  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KioskTracking" \
                   "DisplayName" "무인매장 키오스크 트래킹"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KioskTracking" \
                   "UninstallString" "$\"$INSTDIR\uninstall.exe$\""
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KioskTracking" \
                   "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KioskTracking" \
                   "Publisher" "Hunique"
SectionEnd

Section "Uninstall"
  ; 프로그램 파일만 제거 — 수집 데이터(data\)·설정·로그는 보존
  Delete "$INSTDIR\*.exe"
  Delete "$INSTDIR\*.dll"
  Delete "$INSTDIR\*.onnx"
  Delete "$INSTDIR\*.bat"
  Delete "$INSTDIR\owner_page.html"
  Delete "$INSTDIR\README_설치안내.md"
  Delete "$INSTDIR\CAFE389_세팅안내.md"
  Delete "$INSTDIR\카메라_설치_체크리스트.md"
  RMDir "$INSTDIR"                        ; 데이터가 남아 있으면 폴더는 유지됨

  Delete "$DESKTOP\매장 트래킹 시작.lnk"
  RMDir /r "$SMPROGRAMS\무인매장 트래킹"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KioskTracking"
SectionEnd
