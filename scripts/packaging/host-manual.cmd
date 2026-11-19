@echo off
setlocal DisableDelayedExpansion
pushd "%~dp0"
if errorlevel 1 goto folder_error
if not exist "rexglue.exe" goto missing_exe
set "server_dir=%~dp0userdata\online-server"
setlocal EnableDelayedExpansion
if not exist "!server_dir!" mkdir "!server_dir!"
if not exist "!server_dir!" goto state_error
set "server_address="
set /p "server_address=Public IPv4 or DNS name friends will use (no port): "
if not defined server_address goto address_error
set "server_port=30100"
set /p "server_port=UDP port [30100]: "
set "password=!server_dir!\server-password.txt"
set "invites=!server_dir!\invite-codes.txt"
set "invite_errors=!server_dir!\invite-errors.txt"
set "server_log=!server_dir!\online-server.log"
> "!invites!" echo Friends' code:
if errorlevel 1 goto state_error
rexglue.exe online-invite --address "!server_address!:!server_port!" --password-file "!password!" --create-password >> "!invites!" 2> "!invite_errors!"
if errorlevel 1 goto invite_error
>> "!invites!" echo.
>> "!invites!" echo This PC's code:
rexglue.exe online-invite --address "127.0.0.1:!server_port!" --password-file "!password!" >> "!invites!" 2>> "!invite_errors!"
if errorlevel 1 goto invite_error
type "!invites!"
echo.
echo Keep these codes private. Leave this window open during the test.
start "" notepad.exe "!invites!"
echo The online server is running on UDP !server_port!. Leave this window open; press Ctrl+C to stop. Details: !server_log!
rexglue.exe --log-file "!server_log!" online-server --password-file "!password!" --port "!server_port!" --max-members 8 >nul 2>&1
echo The online server stopped. Close the router port forward after testing.
goto done

:missing_exe
echo ERROR: rexglue.exe is missing. Extract the entire ReRevved release archive, then run host-manual.cmd again.
goto done
:state_error
echo ERROR: Could not create or write the userdata\online-server folder beside this script.
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
