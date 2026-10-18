// Headless Vulkan execution of SPIR-V produced by the production Maxwell translator.
// No game binaries, window, native-memory import or descriptor-indexing features.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s: %d\n", #x, r_); exit(2); } } while (0)
static VkDevice dev;
static VkPhysicalDevice phys;
static void *buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buf, VkDeviceMemory *mem) {
    VkBufferCreateInfo ci = {.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=size,.usage=usage};
    CHECK(vkCreateBuffer(dev,&ci,NULL,buf));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev,*buf,&req);
    VkPhysicalDeviceMemoryProperties props; vkGetPhysicalDeviceMemoryProperties(phys,&props);
    uint32_t type = UINT32_MAX;
    for (uint32_t i=0;i<props.memoryTypeCount;i++) if ((req.memoryTypeBits & (1u<<i)) &&
        (props.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { type=i; break; }
    assert(type != UINT32_MAX);
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
    CHECK(vkAllocateMemory(dev,&ai,NULL,mem)); CHECK(vkBindBufferMemory(dev,*buf,*mem,0));
    void *p; CHECK(vkMapMemory(dev,*mem,0,VK_WHOLE_SIZE,0,&p)); memset(p,0,(size_t)size); return p;
}
int main(int argc,char **argv) {
    assert(argc==2);
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkInstance inst; CHECK(vkCreateInstance(&ici,NULL,&inst));
    uint32_t np=1; CHECK(vkEnumeratePhysicalDevices(inst,&np,&phys)); assert(np);
    uint32_t nq=0; vkGetPhysicalDeviceQueueFamilyProperties(phys,&nq,NULL);
    VkQueueFamilyProperties *families=calloc(nq,sizeof *families); vkGetPhysicalDeviceQueueFamilyProperties(phys,&nq,families);
    uint32_t family=UINT32_MAX;
    for(uint32_t i=0;i<nq;i++)if(families[i].queueFlags&VK_QUEUE_COMPUTE_BIT){family=i;break;}
    free(families);assert(family!=UINT32_MAX);
    float priority=1;VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkPhysicalDeviceFeatures features={.shaderInt64=VK_TRUE};
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,.pEnabledFeatures=&features};
    CHECK(vkCreateDevice(phys,&dci,NULL,&dev));VkQueue queue;vkGetDeviceQueue(dev,family,0,&queue);
    VkBuffer data_buf,map_buf;VkDeviceMemory data_mem,map_mem;
    uint32_t *data=buffer(256,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,&data_buf,&data_mem);
    uint32_t *map=buffer(256,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,&map_buf,&map_mem);
    data[0]=0x80fedcba;data[1]=0x12345678;data[2]=0xc0defeed;data[3]=0x76543210;
    for(int i=8;i<12;i++)data[i]=0x11223344;
    const uint64_t addr=0x7240001000ull;
    map[0]=(uint32_t)addr;map[1]=(uint32_t)(addr>>32);map[2]=64;
    VkDescriptorSetLayoutBinding bindings[17]={{.binding=17,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT}};
    for(int i=0;i<16;i++)bindings[i+1]=(VkDescriptorSetLayoutBinding){.binding=(uint32_t)(64+i),.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo lci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=17,.pBindings=bindings};
    VkDescriptorSetLayout layout;CHECK(vkCreateDescriptorSetLayout(dev,&lci,NULL,&layout));
    VkPipelineLayoutCreateInfo pci={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&layout};
    VkPipelineLayout pl;CHECK(vkCreatePipelineLayout(dev,&pci,NULL,&pl));
    VkDescriptorPoolSize sizes[2]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,16}};
    VkDescriptorPoolCreateInfo dpi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=2,.pPoolSizes=sizes};
    VkDescriptorPool pool;CHECK(vkCreateDescriptorPool(dev,&dpi,NULL,&pool));
    VkDescriptorSetAllocateInfo dai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&layout};
    VkDescriptorSet ds;CHECK(vkAllocateDescriptorSets(dev,&dai,&ds));
    VkDescriptorBufferInfo bi[17];VkWriteDescriptorSet writes[17];
    for(int i=0;i<17;i++){bi[i]=(VkDescriptorBufferInfo){i?data_buf:map_buf,0,256};writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ds,.dstBinding=bindings[i].binding,.descriptorCount=1,.descriptorType=bindings[i].descriptorType,.pBufferInfo=&bi[i]};}
    vkUpdateDescriptorSets(dev,17,writes,0,NULL);
    FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long length=ftell(f);rewind(f);assert(length>0);
    uint32_t *code=malloc((size_t)length);assert(fread(code,1,(size_t)length,f)==(size_t)length);fclose(f);
    VkShaderModuleCreateInfo sci={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)length,.pCode=code};
    VkShaderModule shader;CHECK(vkCreateShaderModule(dev,&sci,NULL,&shader));free(code);
    VkComputePipelineCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.layout=pl,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=shader,.pName="main"}};
    VkPipeline pipe;CHECK(vkCreateComputePipelines(dev,VK_NULL_HANDLE,1,&cpi,NULL,&pipe));
    VkCommandPoolCreateInfo cpci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};
    VkCommandPool cp;CHECK(vkCreateCommandPool(dev,&cpci,NULL,&cp));
    VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cmd;CHECK(vkAllocateCommandBuffers(dev,&cai,&cmd));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(vkBeginCommandBuffer(cmd,&begin));
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipe);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pl,0,1,&ds,0,NULL);vkCmdDispatch(cmd,1,1,1);
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));CHECK(vkQueueWaitIdle(queue));
    for(int i=8;i<12;i++)printf("%08x%c",data[i],i==11?'\n':' ');
    vkDestroyCommandPool(dev,cp,NULL);vkDestroyPipeline(dev,pipe,NULL);vkDestroyShaderModule(dev,shader,NULL);
    vkDestroyDescriptorPool(dev,pool,NULL);vkDestroyPipelineLayout(dev,pl,NULL);vkDestroyDescriptorSetLayout(dev,layout,NULL);
    vkUnmapMemory(dev,data_mem);vkUnmapMemory(dev,map_mem);vkDestroyBuffer(dev,data_buf,NULL);vkDestroyBuffer(dev,map_buf,NULL);vkFreeMemory(dev,data_mem,NULL);vkFreeMemory(dev,map_mem,NULL);
    vkDestroyDevice(dev,NULL);vkDestroyInstance(inst,NULL);
}
