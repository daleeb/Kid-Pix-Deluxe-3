<#
.SYNOPSIS
  Build an ISO (ISO9660 + Joliet + UDF) from a folder with only the built-in IMAPI2FS COM API.
.DESCRIPTION
  No third-party tools. Tested in PowerShell 7.6 and Windows PowerShell 5.1 on Windows 11 26200.
  - FileSystemsToCreate = 7 (ISO9660=1 | Joliet=2 | UDF=4). Windows mounts the UDF tree, so long
    names with spaces/mixed case/Unicode are preserved; Joliet keeps up to 64 chars; ISO9660 is the
    upper-cased fallback (interchange level 2 = 31-char names).
  - FreeMediaBlocks = 0 removes IMAPI's default 650 MB (332800-block) CD capacity limit.
  - StageFiles = $false: IMAPI reads the source files while the image stream is written instead of
    first copying the tree into %TEMP%. Do not modify the source folder until the function returns.
  - The tree is walked here (not with IFsiDirectoryItem.AddTree, which fails on paths >= 260 chars)
    and each file is handed to IMAPI as an IStream opened with a \\?\ long-path prefix.
  - CreateResultImage().ImageStream (COM IStream) is copied to disk in 1 MiB chunks by a small C#
    helper, so memory use stays flat for any image size. The written size is checked against
    TotalBlocks * BlockSize.
.EXAMPLE
  . .\New-IsoFromFolder.ps1
  New-IsoFromFolder -SourceFolder D:\stage\KPD3 -IsoPath D:\out\KidPixDeluxe3.iso -VolumeName KIDPIX_DELUXE3 -Force -Verbose
#>
function New-IsoFromFolder {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$SourceFolder,
        [Parameter(Mandatory)][string]$IsoPath,
        # UDF/Joliet keep this as typed; IMAPI upper-cases and sanitises it for the ISO9660 label.
        [ValidateLength(1, 32)][string]$VolumeName = 'ISO',
        # 0x102 = UDF 1.02 (widest compatibility). 0x150/0x200/0x201/0x250 also accepted by IMAPI.
        [ValidateSet(0x102, 0x150, 0x200, 0x201, 0x250)][int]$UdfRevision = 0x102,
        [switch]$Force
    )
    $ErrorActionPreference = 'Stop'

    $SourceFolder = (Resolve-Path -LiteralPath $SourceFolder).ProviderPath.TrimEnd('\')
    if (-not (Test-Path -LiteralPath $SourceFolder -PathType Container)) { throw "Source folder not found: $SourceFolder" }
    $IsoPath = [IO.Path]::GetFullPath($IsoPath)
    if ((Test-Path -LiteralPath $IsoPath) -and -not $Force) { throw "Target exists (use -Force): $IsoPath" }
    if ($IsoPath.StartsWith($SourceFolder + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'IsoPath must not be inside SourceFolder.'
    }

    if (-not ('IsoBuilderNative' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
public static class IsoBuilderNative {
    [DllImport("shlwapi.dll", CharSet = CharSet.Unicode, PreserveSig = false)]
    private static extern void SHCreateStreamOnFileEx(string pszFile, uint grfMode, uint dwAttributes,
        [MarshalAs(UnmanagedType.Bool)] bool fCreate, IStream pstmTemplate, out IStream ppstm);

    // STGM_READ (0) | STGM_SHARE_DENY_WRITE (0x20)
    public static object OpenRead(string path) {
        string p = path.StartsWith(@"\\?\") ? path :
                   path.StartsWith(@"\\") ? @"\\?\UNC\" + path.Substring(2) : @"\\?\" + path;
        IStream s;
        SHCreateStreamOnFileEx(p, 0x20, 0, false, null, out s);
        return s;
    }

    public static long WriteStream(object comStream, string path) {
        IStream s = (IStream)comStream;
        byte[] buf = new byte[1024 * 1024];
        long total = 0;
        IntPtr pRead = Marshal.AllocHGlobal(sizeof(int));
        try {
            using (FileStream fs = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.None, 1024 * 1024)) {
                while (true) {
                    s.Read(buf, buf.Length, pRead);
                    int n = Marshal.ReadInt32(pRead);
                    if (n <= 0) break;
                    fs.Write(buf, 0, n);
                    total += n;
                }
            }
        } finally { Marshal.FreeHGlobal(pRead); }
        return total;
    }
}
'@
    }

    $fsi = New-Object -ComObject IMAPI2FS.MsftFileSystemImage
    $streams = New-Object System.Collections.Generic.List[object]
    $result = $null
    try {
        $fsi.FileSystemsToCreate     = 7          # ISO9660 + Joliet + UDF
        $fsi.UDFRevision             = $UdfRevision
        $fsi.ISO9660InterchangeLevel = 2
        $fsi.FreeMediaBlocks         = 0          # unlimited
        $fsi.StageFiles              = $false
        $fsi.VolumeName              = $VolumeName

        # Long-path-safe enumeration (\\?\ prefix works in both PS 5.1 and 7).
        $lp = if ($SourceFolder.StartsWith('\\')) { '\\?\UNC\' + $SourceFolder.Substring(2) } else { '\\?\' + $SourceFolder }
        $root = New-Object System.IO.DirectoryInfo($lp)
        $dirs = @($root.EnumerateDirectories('*', [IO.SearchOption]::AllDirectories)) |
                Sort-Object { $_.FullName.Length }   # parents before children
        foreach ($d in $dirs) {
            $rel = $d.FullName.Substring($root.FullName.TrimEnd('\').Length + 1)
            $fsi.Root.AddDirectory($rel)
        }
        foreach ($f in $root.EnumerateFiles('*', [IO.SearchOption]::AllDirectories)) {
            $rel = $f.FullName.Substring($root.FullName.TrimEnd('\').Length + 1)
            $st = [IsoBuilderNative]::OpenRead($f.FullName)
            $streams.Add($st)
            $fsi.Root.AddFile($rel, $st)
        }
        $fileCount = $fsi.FileCount; $dirCount = $fsi.DirectoryCount
        Write-Verbose ("Files={0} Dirs={1} UsedBlocks={2} (~{3:N1} MB)" -f $fileCount, $dirCount, $fsi.UsedBlocks, ($fsi.UsedBlocks * 2048 / 1MB))

        $result = $fsi.CreateResultImage()
        $img = $result.ImageStream
        if ($img -is [System.Management.Automation.PSObject]) { $img = $img.psobject.BaseObject }
        $expected = [int64]$result.TotalBlocks * $result.BlockSize
        $tmp = "$IsoPath.partial"
        $written = [IsoBuilderNative]::WriteStream($img, $tmp)
        if ($written -ne $expected) { Remove-Item -LiteralPath $tmp -Force; throw "ISO size mismatch: wrote $written bytes, expected $expected" }
        Move-Item -LiteralPath $tmp -Destination $IsoPath -Force
    }
    finally {
        foreach ($st in $streams) { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($st) }
        if ($result) { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($result) }
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($fsi)
    }

    [pscustomobject]@{ IsoPath = $IsoPath; SizeBytes = $written; Files = $fileCount; Dirs = $dirCount; Volume = $VolumeName }
}
