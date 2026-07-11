$ErrorActionPreference = 'Stop'

$path = Join-Path -Path (Get-Location).Path `
    -ChildPath 'mDNSWindows\SystemService\mDNSResponder.vcxproj'
$source = Get-Content -Raw -Path $path

$legacyDefine = '_LEGACY_NAT_TRAVERSAL_;'
$defineCount = ([regex]::Matches($source, [regex]::Escape($legacyDefine))).Count
if ($defineCount -ne 6) {
    throw "Expected 6 legacy NAT definitions in $path, found $defineCount"
}
$source = $source.Replace($legacyDefine, '')

$sourcePattern = '(?m)^\s*<ClCompile Include="\.\.\\\.\.\\mDNSMacOSX\\LegacyNATTraversal\.c" />\r?\n'
$sourceMatches = [regex]::Matches($source, $sourcePattern)
if ($sourceMatches.Count -ne 1) {
    throw "Expected one LegacyNATTraversal source entry in $path, found $($sourceMatches.Count)"
}
$source = [regex]::Replace($source, $sourcePattern, '')

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($path, $source, $utf8NoBom)
