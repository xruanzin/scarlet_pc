param(
    [string]$OutputDir = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build\tools\vulkan-loader-import')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Import-MsvcEnvironment {
    if (Get-Command lib.exe -ErrorAction SilentlyContinue) { return }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere -PathType Leaf)) {
        throw 'Vulkan loader import library requires MSVC Build Tools; vswhere.exe was not found.'
    }

    $vsRoot = (& $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath).Trim()
    if ([string]::IsNullOrWhiteSpace($vsRoot)) {
        throw 'Visual Studio C++ Build Tools were not found.'
    }

    $vsDevCmd = Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat'
    $envLines = & cmd.exe /s /c "`"$vsDevCmd`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
    foreach ($line in $envLines) {
        $p = $line.IndexOf('=')
        if ($p -gt 0) {
            Set-Item -Path ("Env:" + $line.Substring(0, $p)) -Value $line.Substring($p + 1)
        }
    }

    if (-not (Get-Command lib.exe -ErrorAction SilentlyContinue)) {
        throw 'MSVC environment loaded but lib.exe is still unavailable.'
    }
}

Import-MsvcEnvironment
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$def = Join-Path $OutputDir 'vulkan-1.def'
$lib = Join-Path $OutputDir 'vulkan-1.lib'

@'
LIBRARY vulkan-1.dll
EXPORTS
    vkAllocateDescriptorSets
    vkAllocateMemory
    vkBindBufferMemory
    vkBindImageMemory
    vkCmdBindDescriptorSets
    vkCmdBindPipeline
    vkCmdClearColorImage
    vkCmdCopyBuffer
    vkCmdDispatch
    vkCmdFillBuffer
    vkCmdPipelineBarrier
    vkCreateBuffer
    vkCreateComputePipelines
    vkCreateDescriptorPool
    vkCreateDescriptorSetLayout
    vkCreateImage
    vkCreateImageView
    vkCreatePipelineLayout
    vkCreateSampler
    vkCreateShaderModule
    vkDestroyBuffer
    vkDestroyDescriptorPool
    vkDestroyDescriptorSetLayout
    vkDestroyImage
    vkDestroyImageView
    vkDestroyPipeline
    vkDestroyPipelineLayout
    vkDestroySampler
    vkDestroyShaderModule
    vkDeviceWaitIdle
    vkEnumerateDeviceExtensionProperties
    vkFreeMemory
    vkGetBufferMemoryRequirements
    vkGetImageMemoryRequirements
    vkGetPhysicalDeviceFeatures2
    vkGetPhysicalDeviceFormatProperties
    vkGetPhysicalDeviceMemoryProperties
    vkGetPhysicalDeviceProperties
    vkGetPhysicalDeviceProperties2
    vkGetPhysicalDeviceQueueFamilyProperties
    vkMapMemory
    vkResetDescriptorPool
    vkUnmapMemory
    vkUpdateDescriptorSets
'@ | Set-Content $def -Encoding ASCII

& lib.exe /nologo /machine:x64 "/def:$def" "/out:$lib"
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $lib -PathType Leaf)) {
    throw 'Could not generate the Vulkan loader import library for the FSR4 provider.'
}

Write-Host "[Scarlet PC] Vulkan loader import library ready: $lib" -ForegroundColor Green
