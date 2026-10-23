@echo off
setlocal DisableDelayedExpansion
pushd "%~dp0"
if errorlevel 1 goto folder_error
if not exist "rexglue.exe" goto missing_exe
set "documents="
for /f "usebackq delims=" %%D in (`powershell -NoProfile -Command "[Environment]::GetFolderPath('MyDocuments')"`) do set "documents=%%D"
if not defined documents goto documents_error
set "relay_dir=%documents%\My Games\ReRevved\relay"
setlocal EnableDelayedExpansion
if not exist "!relay_dir!" mkdir "!relay_dir!"
if not exist "!relay_dir!" goto state_error
set "relay_address="
set /p "relay_address=Public IPv4 or DNS name friends will use (no port): "
if not defined relay_address goto address_error
set "relay_port=30100"
set /p "relay_port=UDP port [30100]: "
set "secret=!relay_dir!\relay-secret.txt"
set "invites=!relay_dir!\invite-codes.txt"
set "invite_errors=!relay_dir!\invite-errors.txt"
set "relay_log=!relay_dir!\relay.log"
> "!invites!" echo Friends' code:
if errorlevel 1 goto state_error
rexglue.exe relay-invite --address "!relay_address!:!relay_port!" --secret-file "!secret!" --create-secret >> "!invites!" 2> "!invite_errors!"
if errorlevel 1 goto invite_error
>> "!invites!" echo.
>> "!invites!" echo This PC's code:
rexglue.exe relay-invite --address "127.0.0.1:!relay_port!" --secret-file "!secret!" >> "!invites!" 2>> "!invite_errors!"
if errorlevel 1 goto invite_error
type "!invites!"
echo.
echo Keep these codes private. Leave this window open during the test.
start "" notepad.exe "!invites!"
echo Relay running on UDP !relay_port!. Leave this window open; press Ctrl+C to stop. Details: !relay_log!
rexglue.exe --log-file "!relay_log!" relay --secret-file "!secret!" --port "!relay_port!" --max-members 8 >nul 2>&1
echo Relay stopped. Close the router port forward after testing.
goto done

:missing_exe
echo ERROR: rexglue.exe is missing. Extract the entire ReRevved release archive, then run host-manual.cmd again.
goto done
:documents_error
echo ERROR: Could not resolve the Windows Documents folder.
goto done
:state_error
echo ERROR: Could not create or write the relay folder in Documents.
goto done
:address_error
echo No address entered. Find your public IPv4 in your router's Internet status.
echo See ONLINE.txt, then run this script again.
goto done
:invite_error
echo ERROR: Could not create invite codes. Check the address and port.
echo Details: !invite_errors!
echo Do not use the incomplete invite-codes.txt. Run this script again.
goto done
:folder_error
echo ERROR: Could not open the folder containing this script.
pause
exit /b 1
:done
popd
pause
