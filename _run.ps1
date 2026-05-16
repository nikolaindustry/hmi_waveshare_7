$env:PATH = "C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin;C:\Espressif\tools\python\v6.0.1\venv\Scripts;" + $env:PATH
$env:IDF_PATH = "C:\esp\v6.0.1\esp-idf"
$env:IDF_TOOLS_PATH = "C:\Espressif\tools"
Set-Location "c:\Users\user\Downloads\esp32_p4_deom\hyperwisor_s3"

Get-Process python -ErrorAction SilentlyContinue | Where-Object { (Get-CimInstance Win32_Process -Filter "ProcessId=$($_.Id)" -EA SilentlyContinue).CommandLine -match 'serial.tools|miniterm' } | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800

Write-Host "=== BUILD ==="
cmake --build build *> build.log
$bc = $LASTEXITCODE
Get-Content build.log | Select-Object -Last 8
Write-Host "BuildExit=$bc"
if ($bc -ne 0) { Write-Host "BUILD FAILED"; Get-Content build.log -Tail 40; exit 1 }

Write-Host "`n=== FLASH ==="
python -m esptool --chip esp32s3 -p COM4 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0x20000 build/hyperwisor_s3.bin *> flash.log
$fc = $LASTEXITCODE
Get-Content flash.log | Select-Object -Last 6
Write-Host "FlashExit=$fc"
if ($fc -ne 0) { Write-Host "FLASH FAILED"; exit 1 }

Write-Host "`n=== LOG (35s) ==="
if (Test-Path serial.log) { Remove-Item serial.log -Force }
$p = Start-Process -FilePath "python" -ArgumentList "-m","serial.tools.miniterm","COM4","115200","--raw" -RedirectStandardOutput serial.log -NoNewWindow -PassThru
Start-Sleep -Seconds 35
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

Write-Host "`n=== WS / TLS LINES ==="
Get-Content serial.log -ErrorAction SilentlyContinue | Select-String -Pattern 'HYPER_WS|HYPER_CORE|websocket|mbedtls|handshake|CONNECTED|DISCONNECT|tls_error|TLS|SSL|alert|ERROR|Certificate|ClientHello|ServerHello|ssl_|handshake' | ForEach-Object { $_.Line } | Select-Object -Last 100
