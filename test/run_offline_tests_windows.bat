@echo off
rem Build and run the standalone offline encoder, decoder and conversion tests with
rem MSVC. Run from anywhere; takes the test names to run, or all of them by default:
rem
rem   test\run_offline_tests_windows.bat
rem   test\run_offline_tests_windows.bat offline_decode_test
rem
rem Needs a Visual Studio with the C++ tools, and the prebuilt Xiph libraries the build
rem hook downloads into .dart_tool\flutter_soloud\xiph\prebuild, so build the example
rem for Windows once first (cd example ^&^& flutter build windows).
setlocal

set REPO=%~dp0..
set XIPH=%REPO%\.dart_tool\flutter_soloud\xiph\prebuild
if not exist "%XIPH%\windows\x64\FLAC.lib" (
  echo Missing %XIPH%\windows\x64: run 'flutter build windows' in example\ first. 1>&2
  exit /b 1
)

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set VS=%%i
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul

set OUT=%TEMP%\flutter_soloud_offline_tests
if not exist "%OUT%" mkdir "%OUT%"

set TESTS=%*
if "%TESTS%"=="" set TESTS=offline_encode_test offline_decode_test offline_convert_test

set SOURCES="%REPO%\src\mixeroutput\offline_decoder.cpp" ^
 "%REPO%\src\mixeroutput\offline_encoder.cpp" ^
 "%REPO%\src\mixeroutput\mixer_output_encoder.cpp" ^
 "%REPO%\src\mixeroutput\opus_output_encoder.cpp" ^
 "%REPO%\src\mixeroutput\vorbis_output_encoder.cpp" ^
 "%REPO%\src\mixeroutput\flac_output_encoder.cpp" ^
 "%REPO%\src\mixeroutput\wav_output_encoder.cpp" ^
 "%REPO%\src\mixeroutput\sinc_resampler.cpp" ^
 "%REPO%\src\audiobuffer\stream_decoder.cpp" ^
 "%REPO%\src\audiobuffer\aac_stream_decoder.cpp" ^
 "%REPO%\src\audiobuffer\aac_stream_decoder_windows.cpp" ^
 "%REPO%\src\audiobuffer\flac_stream_decoder.cpp" ^
 "%REPO%\src\audiobuffer\m4a_metadata.cpp" ^
 "%REPO%\src\audiobuffer\mp3_stream_decoder.cpp" ^
 "%REPO%\src\audiobuffer\ogg_flac_stream_decoder.cpp" ^
 "%REPO%\src\audiobuffer\opus_stream_decoder.cpp" ^
 "%REPO%\src\audiobuffer\vorbis_stream_decoder.cpp" ^
 "%REPO%\src\audiobuffer\wav_stream_decoder.cpp" ^
 "%REPO%\src\native_decoder\os_decoder_windows.cpp" ^
 "%REPO%\src\soloud\src\audiosource\wav\dr_impl.cpp" ^
 "%REPO%\src\soloud\src\core\soloud_file.cpp"

set FLAGS=/nologo /EHsc /std:c++17 /O2 /MD /utf-8 /W1 /D_USE_MATH_DEFINES /DNOMINMAX ^
 /D_CRT_SECURE_NO_WARNINGS /I"%REPO%\src" /I"%REPO%\src\soloud\include" ^
 /I"%REPO%\src\native_decoder" /I"%XIPH%\include"

set OBJ=%OUT%\obj
if not exist "%OBJ%" mkdir "%OBJ%"
cl %FLAGS% /c /Fo"%OBJ%\\" %SOURCES% >"%OUT%\build.log" 2>&1
if errorlevel 1 (
  type "%OUT%\build.log"
  exit /b 1
)

rem Tests read %TMPDIR% for where to put their files, as they do on the other platforms.
set TMPDIR=%OUT%\
set PATH=%XIPH%\windows\x64;%PATH%

rem The encode test brings its own dr_wav, so it links the encoders and nothing else.
set ENCODERS="%OBJ%\offline_encoder.obj" "%OBJ%\mixer_output_encoder.obj" ^
 "%OBJ%\opus_output_encoder.obj" "%OBJ%\vorbis_output_encoder.obj" ^
 "%OBJ%\flac_output_encoder.obj" "%OBJ%\wav_output_encoder.obj" "%OBJ%\sinc_resampler.obj"

set FAILED=0
setlocal enabledelayedexpansion
for %%t in (%TESTS%) do (
  echo === %%t
  set OBJECTS="%OBJ%\*.obj"
  if "%%t"=="offline_encode_test" set OBJECTS=%ENCODERS%
  cl %FLAGS% /Fe"%OUT%\%%t.exe" /Fo"%OUT%\%%t.obj" "%REPO%\test\%%t.cpp" !OBJECTS! ^
    /link /LIBPATH:"%XIPH%\windows\x64" FLAC.lib ogg.lib opus.lib vorbis.lib vorbisenc.lib ^
    vorbisfile.lib mfplat.lib mfreadwrite.lib mfuuid.lib shlwapi.lib ole32.lib psapi.lib ^
    >"%OUT%\%%t.log" 2>&1
  if errorlevel 1 (
    type "%OUT%\%%t.log"
    set FAILED=1
  ) else (
    "%OUT%\%%t.exe"
    if errorlevel 1 set FAILED=1
  )
)
exit /b !FAILED!
