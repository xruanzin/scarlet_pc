#!/usr/bin/env python3
"""Exercise production device negotiation against a synthetic Vulkan ICD."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_stencil_sampling import function
from test_transform_feedback import compiler_command
ROOT=Path(__file__).resolve().parent.parent
HARNESS=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn/vulkan/vk.h"
#undef FATAL
#define FATAL(...) do { fprintf(stderr,__VA_ARGS__); exit(77); } while(0)
VkG g_vk;
static bool g_fault_ext;
static int mode;
static unsigned loads,created;
#define DEFINE(name,suffix) PFN_##name lvk_##name;
VK_COMPAT_COMMANDS(DEFINE)
#undef DEFINE
static void stub(void) {}
PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice d,const char *name) {
    bool suffix=strstr(name,"KHR")||strstr(name,"EXT");
    assert(suffix==(g_vk.api_version<VK_API_VERSION_1_3));
    loads++;
    return mode==3 && loads==2 ? NULL : (PFN_vkVoidFunction)stub;
}
VkResult vkEnumerateDeviceExtensionProperties(VkPhysicalDevice p,const char *layer,uint32_t *n,VkExtensionProperties *out) {
    const char *names[]={VK_KHR_SWAPCHAIN_EXTENSION_NAME,VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
      VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME,
      VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME,VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME,VK_EXT_DEVICE_FAULT_EXTENSION_NAME};
    *n=mode==1?4:(mode==4?8:5);
    if(out)for(unsigned i=0;i<*n;i++)strcpy(out[i].extensionName,names[i]);
    return VK_SUCCESS;
}
void vkGetPhysicalDeviceFeatures2(VkPhysicalDevice p,VkPhysicalDeviceFeatures2 *f) {
    f->features.shaderInt64=f->features.independentBlend=f->features.textureCompressionBC=VK_TRUE;
    for(VkBaseOutStructure *s=f->pNext;s;s=s->pNext)switch(s->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
        ((VkPhysicalDeviceVulkan12Features*)s)->timelineSemaphore=VK_TRUE;break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR:
        ((VkPhysicalDeviceDynamicRenderingFeaturesKHR*)s)->dynamicRendering=mode==2?VK_FALSE:VK_TRUE;break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR:
        ((VkPhysicalDeviceSynchronization2FeaturesKHR*)s)->synchronization2=VK_TRUE;break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT:
        ((VkPhysicalDeviceExtendedDynamicStateFeaturesEXT*)s)->extendedDynamicState=VK_TRUE;break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT:
        ((VkPhysicalDeviceExtendedDynamicState2FeaturesEXT*)s)->extendedDynamicState2=VK_TRUE;break;
      default:break;
    }
}
void vkGetPhysicalDeviceProperties2(VkPhysicalDevice p,VkPhysicalDeviceProperties2 *f) { assert(!"unexpected optional query"); }
VkResult vkCreateDevice(VkPhysicalDevice p,const VkDeviceCreateInfo *ci,const VkAllocationCallbacks *a,VkDevice *d) {
    bool compat=g_vk.api_version<VK_API_VERSION_1_3,have13=false,have_dynamic=false;
    const VkPhysicalDeviceFeatures2 *f=ci->pNext;
    for(const VkBaseInStructure *s=f->pNext;s;s=s->pNext) {
      if(s->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)have13=true;
      if(s->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR)have_dynamic=true;
      if(compat && s->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
        const VkPhysicalDeviceVulkan12Features *v=(const void*)s;
        assert(v->timelineSemaphore && !v->shaderInt8 && !v->storageBuffer8BitAccess && !v->scalarBlockLayout);
      }
      if(compat && s->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES)
        assert(!((const VkPhysicalDeviceVulkan11Features*)s)->storageBuffer16BitAccess);
    }
    assert(have13==!compat && have_dynamic==compat);
    assert(ci->enabledExtensionCount==(compat?5:1));
    created++; *d=(VkDevice)1; return VK_SUCCESS;
}
void vkGetDeviceQueue(VkDevice d,uint32_t q,uint32_t i,VkQueue *out) {*out=(VkQueue)1;}
VkResult vmaCreateAllocator(const VmaAllocatorCreateInfo *ci,VmaAllocator *out) {
    uint32_t expected=g_vk.api_version;
#if VMA_VULKAN_VERSION < 1004000
    if(expected>VK_API_VERSION_1_3)expected=VK_API_VERSION_1_3;
#endif
    assert(ci->vulkanApiVersion==expected); *out=(VmaAllocator)1;return VK_SUCCESS;
}
VkResult vkCreateSemaphore(VkDevice d,const VkSemaphoreCreateInfo *ci,const VkAllocationCallbacks *a,VkSemaphore *out){*out=(VkSemaphore)1;return VK_SUCCESS;}
VkResult vkCreateCommandPool(VkDevice d,const VkCommandPoolCreateInfo *ci,const VkAllocationCallbacks *a,VkCommandPool *out){*out=(VkCommandPool)1;return VK_SUCCESS;}
VkResult vkCreatePipelineCache(VkDevice d,const VkPipelineCacheCreateInfo *ci,const VkAllocationCallbacks *a,VkPipelineCache *out){*out=(VkPipelineCache)1;return VK_SUCCESS;}
void vk_device_fault_report(void) {assert(!"unexpected device loss");}
/*FUNCTIONS*/
int main(int argc,char **argv) {
    assert(instance_api_for(VK_API_VERSION_1_4,true,false)==VK_API_VERSION_1_2);
    assert(instance_api_for(VK_API_VERSION_1_2,true,false)==VK_API_VERSION_1_2);
    assert(instance_api_for(VK_API_VERSION_1_3,false,false)==VK_API_VERSION_1_3);
    assert(instance_api_for(VK_API_VERSION_1_3,false,true)==VK_API_VERSION_1_3);
    assert(instance_api_for(VK_API_VERSION_1_4,false,false)==VK_API_VERSION_1_3);
    assert(instance_api_for(VK_API_VERSION_1_4,false,true)==VK_API_VERSION_1_3);
    assert(minimum_device_api(true,false)==VK_API_VERSION_1_2);
    assert(minimum_device_api(false,false)==VK_API_VERSION_1_3);
    assert(minimum_device_api(false,true)==VK_API_VERSION_1_3);
    mode=atoi(argv[1]);g_vk.api_version=atoi(argv[2])==12?VK_API_VERSION_1_2:atoi(argv[2])==14?VK_API_VERSION_1_4:VK_API_VERSION_1_3;
    create_device();
    assert(created==1 && loads==14);
    assert((strstr(pipeline_cache_path(),"dzn12")!=NULL)==(g_vk.api_version<VK_API_VERSION_1_3));
    return 0;
}
'''
class Vulkan12Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        include=Path(os.environ.get('VULKAN_INCLUDE','/usr/include'))
        if not (include/'vulkan/vulkan.h').exists():raise unittest.SkipTest('Vulkan headers required')
        cls.tmp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.tmp.cleanup);cls.path=Path(cls.tmp.name)
        source=(ROOT/'runtime/nvn/vulkan/device.c').read_text()
        funcs='\n'.join(function(source,n) for n in ('instance_api_for','minimum_device_api','load_compat_commands','has_ext','pipeline_cache_path','create_device'))
        src=cls.path/'device.c';src.write_text(HARNESS.replace('/*FUNCTIONS*/',funcs));cls.exe=cls.path/'device'
        run=subprocess.run(compiler_command()+['-O1','-UNDEBUG','-I'+str(ROOT/'runtime'),'-I'+str(include),
            '-I'+os.environ.get('VMA_INCLUDE',str(include)),str(src),'-o',str(cls.exe)],capture_output=True,text=True)
        if run.returncode:raise AssertionError(run.stderr)
    def run_case(self,mode,api=12):
        return subprocess.run([str(self.exe),str(mode),str(api)],cwd=self.path,capture_output=True,text=True)
    def test_extension_negotiation_and_core_path(self):
        for api in (12,13,14):
            run=self.run_case(0,api);self.assertEqual(run.returncode,0,run.stdout+run.stderr)
    def test_optional_extension_without_feature_is_not_enabled(self):
        run=self.run_case(4);self.assertEqual(run.returncode,0,run.stdout+run.stderr)
    def test_missing_extension_is_rejected_before_device_creation(self):
        run=self.run_case(1);self.assertEqual(run.returncode,77);self.assertIn('missing required extension',run.stderr)
    def test_missing_feature_is_rejected_before_device_creation(self):
        run=self.run_case(2);self.assertEqual(run.returncode,77);self.assertIn('dynamic.dynamicRendering',run.stderr)
    def test_missing_extension_entrypoint_is_rejected(self):
        run=self.run_case(3);self.assertEqual(run.returncode,77);self.assertIn('required Vulkan commands unavailable',run.stderr)
if __name__=='__main__':unittest.main(verbosity=2)
