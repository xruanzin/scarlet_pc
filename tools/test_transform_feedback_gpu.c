// Execute generated TF shaders and read back all three streams, without game assets.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#define CHECK(call) do { VkResult r = (call); if (r != VK_SUCCESS) { \
    fprintf(stderr, "%s: %d\n", #call, r); return 2; } } while (0)

int main(int argc, char **argv) {
    assert(argc == 2);
    VkApplicationInfo app = {.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkInstance inst; VkResult instance_result=vkCreateInstance(&ici,NULL,&inst);
    if (instance_result==VK_ERROR_INCOMPATIBLE_DRIVER) {
        fprintf(stderr,"no installed Vulkan driver\n"); return 77;
    }
    if (instance_result!=VK_SUCCESS) { fprintf(stderr,"vkCreateInstance: %d\n",instance_result); return 2; }
    uint32_t count=0; CHECK(vkEnumeratePhysicalDevices(inst,&count,NULL)); assert(count);
    VkPhysicalDevice *devices=calloc(count,sizeof *devices);
    CHECK(vkEnumeratePhysicalDevices(inst,&count,devices));
    VkPhysicalDevice phys=VK_NULL_HANDLE;
    for (uint32_t i=0;i<count;i++) {
        VkPhysicalDeviceTransformFeedbackFeaturesEXT tf={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 f={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&tf};
        vkGetPhysicalDeviceFeatures2(devices[i],&f);
        if (tf.transformFeedback) { phys=devices[i]; break; }
    }
    free(devices);
    if (!phys) { fprintf(stderr,"no VK_EXT_transform_feedback device\n"); vkDestroyInstance(inst,NULL); return 77; }
    VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(phys,&properties);
    fprintf(stderr,"TF readback device: %s\n",properties.deviceName);
    uint32_t nq=0; vkGetPhysicalDeviceQueueFamilyProperties(phys,&nq,NULL);
    VkQueueFamilyProperties *families=calloc(nq,sizeof *families);
    vkGetPhysicalDeviceQueueFamilyProperties(phys,&nq,families);
    uint32_t family=UINT32_MAX;
    for (uint32_t i=0;i<nq;i++) if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family=i; break; }
    free(families); assert(family!=UINT32_MAX);
    float priority=1;
    VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    const char *extension=VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME;
    VkPhysicalDeviceTransformFeedbackFeaturesEXT tf={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,.transformFeedback=VK_TRUE};
    VkPhysicalDeviceFeatures features; vkGetPhysicalDeviceFeatures(phys,&features);
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&tf,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,
        .enabledExtensionCount=1,.ppEnabledExtensionNames=&extension,.pEnabledFeatures=&features};
    VkDevice dev; CHECK(vkCreateDevice(phys,&dci,NULL,&dev));
    VkQueue queue; vkGetDeviceQueue(dev,family,0,&queue);
    PFN_vkCmdBindTransformFeedbackBuffersEXT bind=(PFN_vkCmdBindTransformFeedbackBuffersEXT)vkGetDeviceProcAddr(dev,"vkCmdBindTransformFeedbackBuffersEXT");
    PFN_vkCmdBeginTransformFeedbackEXT begin=(PFN_vkCmdBeginTransformFeedbackEXT)vkGetDeviceProcAddr(dev,"vkCmdBeginTransformFeedbackEXT");
    PFN_vkCmdEndTransformFeedbackEXT end=(PFN_vkCmdEndTransformFeedbackEXT)vkGetDeviceProcAddr(dev,"vkCmdEndTransformFeedbackEXT");
    assert(bind && begin && end);
    VkBufferCreateInfo bci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=768,.usage=VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT};
    VkBuffer buffer; CHECK(vkCreateBuffer(dev,&bci,NULL,&buffer));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev,buffer,&req);
    VkPhysicalDeviceMemoryProperties mem; vkGetPhysicalDeviceMemoryProperties(phys,&mem);
    uint32_t type=UINT32_MAX;
    for (uint32_t i=0;i<mem.memoryTypeCount;i++)
        if ((req.memoryTypeBits & (1u<<i)) && (mem.memoryTypes[i].propertyFlags &
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { type=i; break; }
    assert(type!=UINT32_MAX);
    VkMemoryAllocateInfo mai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
    VkDeviceMemory memory; CHECK(vkAllocateMemory(dev,&mai,NULL,&memory)); CHECK(vkBindBufferMemory(dev,buffer,memory,0));
    uint32_t *pixels; CHECK(vkMapMemory(dev,memory,0,VK_WHOLE_SIZE,0,(void **)&pixels)); memset(pixels,0xcd,768);
    FILE *f=fopen(argv[1],"rb"); assert(f); fseek(f,0,SEEK_END); long size=ftell(f); rewind(f);
    uint32_t *code=malloc((size_t)size); assert(fread(code,1,(size_t)size,f)==(size_t)size); fclose(f);
    VkShaderModuleCreateInfo smci={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)size,.pCode=code};
    VkShaderModule shader; CHECK(vkCreateShaderModule(dev,&smci,NULL,&shader)); free(code);
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS};
    VkRenderPassCreateInfo rpci={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.subpassCount=1,.pSubpasses=&subpass};
    VkRenderPass pass; CHECK(vkCreateRenderPass(dev,&rpci,NULL,&pass));
    VkFramebufferCreateInfo fbci={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=pass,.width=1,.height=1,.layers=1};
    VkFramebuffer fb; CHECK(vkCreateFramebuffer(dev,&fbci,NULL,&fb));
    VkPipelineLayoutCreateInfo lci={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(dev,&lci,NULL,&layout));
    VkPipelineShaderStageCreateInfo stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=shader,.pName="main"};
    VkPipelineVertexInputStateCreateInfo vert={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_POINT_LIST};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.rasterizerDiscardEnable=VK_TRUE,.lineWidth=1};
    VkGraphicsPipelineCreateInfo pci={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=1,.pStages=&stage,
        .pVertexInputState=&vert,.pInputAssemblyState=&ia,.pRasterizationState=&raster,.layout=layout,.renderPass=pass};
    VkPipeline pipeline; CHECK(vkCreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&pci,NULL,&pipeline));
    VkCommandPoolCreateInfo cpci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};
    VkCommandPool pool; CHECK(vkCreateCommandPool(dev,&cpci,NULL,&pool));
    VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(dev,&cai,&cmd));
    VkCommandBufferBeginInfo cbbi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd,&cbbi));
    VkRenderPassBeginInfo rb={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,.framebuffer=fb,.renderArea={{0,0},{1,1}}};
    vkCmdBeginRenderPass(cmd,&rb,VK_SUBPASS_CONTENTS_INLINE); vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    VkBuffer buffers[]={buffer,buffer,buffer}; VkDeviceSize offsets[]={0,256,512},sizes[]={256,256,256};
    bind(cmd,0,3,buffers,offsets,sizes); begin(cmd,0,0,NULL,NULL); vkCmdDraw(cmd,4,1,0,0); end(cmd,0,0,NULL,NULL);
    vkCmdEndRenderPass(cmd);
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
    CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
    CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
    for (int stream=0;stream<3;stream++) for (int i=0;i<16;i++) printf("%08x%c",pixels[stream*64+i],i==15?'\n':' ');
    vkDestroyCommandPool(dev,pool,NULL); vkDestroyPipeline(dev,pipeline,NULL); vkDestroyPipelineLayout(dev,layout,NULL);
    vkDestroyFramebuffer(dev,fb,NULL); vkDestroyRenderPass(dev,pass,NULL); vkDestroyShaderModule(dev,shader,NULL);
    vkUnmapMemory(dev,memory); vkDestroyBuffer(dev,buffer,NULL); vkFreeMemory(dev,memory,NULL);
    vkDestroyDevice(dev,NULL); vkDestroyInstance(inst,NULL); return 0;
}
