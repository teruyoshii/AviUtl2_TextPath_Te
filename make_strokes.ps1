# KanjiVGのXML(kanjivg-日付.xml)から、プラグインが読む筆順データ TextPath_TeStrokes.txt を作る
# 使い方: .\make_strokes.ps1 展開したxmlのパス
# 出力の書式: 1行1文字で「文字コード(16進) タブ 1画目のパス タブ 2画目のパス …」。先頭の#の行は注記
param([Parameter(Mandatory)][string]$Xml)
$text = [IO.File]::ReadAllText($Xml, [Text.Encoding]::UTF8)
$out = [Text.StringBuilder]::new()
[void]$out.AppendLine('# KanjiVG (https://kanjivg.tagaini.net) Copyright (C) 2009-2026 Ulrich Apel.')
[void]$out.AppendLine('# CC BY-SA 3.0 (https://creativecommons.org/licenses/by-sa/3.0/). 元のXMLから文字コードと画のパスだけを取り出して変換したもの。')
# 異体字(kvg:kanji_XXXXX-Kaisho等)は使わない
foreach ($m in [regex]::Matches($text, '(?s)<kanji id="kvg:kanji_([0-9a-f]+)">(.*?)</kanji>')) {
	$paths = [regex]::Matches($m.Groups[2].Value, '<path [^>]*?\bd="([^"]+)"') | ForEach-Object { $_.Groups[1].Value }
	if (-not $paths) { continue }
	[void]$out.Append($m.Groups[1].Value.TrimStart('0')).Append("`t").AppendLine(($paths -join "`t"))
}
[IO.File]::WriteAllText("$PSScriptRoot\TextPath_TeStrokes.txt", $out.ToString().Replace("`r`n", "`n"), [Text.UTF8Encoding]::new($false))
