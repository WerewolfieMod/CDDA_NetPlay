# DDA MP build tooling: install VS 2022 Build Tools with the C++ workload
# into D:\VS2022BuildTools via the Visual Studio Installer's setup.exe directly.
$ErrorActionPreference = 'Stop'
$setup = 'W:\Program Files (x86)\Microsoft Visual Studio\Installer\setup.exe'
$manifest = 'D:\Games\CataDDA\cdda-src\channel-manifest.json'
$logFile = 'D:\Games\CataDDA\cdda-src\vsbt-install-d.log'

& $setup install `
    --quiet --norestart `
    --installPath 'D:\VS2022BuildTools' `
    --channelUri "$manifest" `
    --channelId 'VisualStudio.17.Release' `
    --productId 'Microsoft.VisualStudio.Product.BuildTools' `
    --add 'Microsoft.VisualStudio.Workload.VCTools' `
    --includeRecommended

$code = $LASTEXITCODE
Add-Content -Path $logFile -Value "launcher ExitCode: $code at $(Get-Date -Format 'HH:mm:ss')"
exit $code
