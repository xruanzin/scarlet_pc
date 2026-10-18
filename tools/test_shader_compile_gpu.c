// Render a register/pc-dispatch shader to integer pixels, with actual compiled vertex and fragment modules.
// Invoked by test_shader_compilation.py; no game files, window or proprietary GPU is required.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    fprintf(stderr, "%s: %d\n", #x, r_); exit(2); } } while (0)
static VkDevice dev;
static VkPhysicalDevice phys;

static uint32_t memory_type(uint32_t mask, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties p; vkGetPhysicalDeviceMemoryProperties(phys, &p);
    for (uint32_t i = 0; i < p.memoryTypeCount; i++)
        if ((mask & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags) return i;
    assert(!"memory type unavailable"); return 0;
}
static VkShaderModule module(const char *path) {
    FILE *f = fopen(path, "rb"); assert(f); fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    assert(n > 20 && n % 4 == 0); uint32_t *code = malloc((size_t)n);
    assert(fread(code, 1, (size_t)n, f) == (size_t)n); fclose(f);
    VkShaderModuleCreateInfo ci = {.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)n,.pCode=code};
    VkShaderModule m; CHECK(vkCreateShaderModule(dev, &ci, NULL, &m)); free(code); return m;
}
int main(int argc, char **argv) {
    assert(argc == 4 || argc == 5);
    VkApplicationInfo app = {.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkInstance inst; CHECK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t count = 1; CHECK(vkEnumeratePhysicalDevices(inst, &count, &phys)); assert(count);
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
    VkQueueFamilyProperties *families = calloc(nq, sizeof *families);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, families);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = i; break; }
    free(families); assert(family != UINT32_MAX);
    float priority = 1;
    VkDeviceQueueCreateInfo qi = {.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,
        .queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo dci = {.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi};
    CHECK(vkCreateDevice(phys, &dci, NULL, &dev)); VkQueue queue; vkGetDeviceQueue(dev, family, 0, &queue);

    VkBufferCreateInfo bci = {.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=32,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buffer; CHECK(vkCreateBuffer(dev, &bci, NULL, &buffer));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev, buffer, &req);
    VkMemoryAllocateInfo ai = {.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,
        .memoryTypeIndex=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory bmem; CHECK(vkAllocateMemory(dev, &ai, NULL, &bmem)); CHECK(vkBindBufferMemory(dev, buffer, bmem, 0));
    uint32_t *pixels; CHECK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&pixels)); memset(pixels, 0, 32);
    VkImageCreateInfo imci = {.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_R32G32B32A32_UINT,.extent={2,1,1},.mipLevels=1,.arrayLayers=1,.samples=1,
        .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VkImage image; CHECK(vkCreateImage(dev, &imci, NULL, &image)); vkGetImageMemoryRequirements(dev, image, &req);
    ai.allocationSize = req.size; ai.memoryTypeIndex = memory_type(req.memoryTypeBits, 0);
    VkDeviceMemory imem; CHECK(vkAllocateMemory(dev, &ai, NULL, &imem)); CHECK(vkBindImageMemory(dev, image, imem, 0));
    VkImageViewCreateInfo vi = {.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image,.viewType=VK_IMAGE_VIEW_TYPE_2D,
        .format=imci.format,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VkImageView view; CHECK(vkCreateImageView(dev, &vi, NULL, &view));
    VkAttachmentDescription attachment = {.format=imci.format,.samples=1,.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp=VK_ATTACHMENT_STORE_OP_STORE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference ref = {0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&ref};
    VkSubpassDependency deps[] = {
        {.srcSubpass=VK_SUBPASS_EXTERNAL,.dstSubpass=0,.srcStageMask=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
         .dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT},
        {.srcSubpass=0,.dstSubpass=VK_SUBPASS_EXTERNAL,.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         .dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT,.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT}};
    VkRenderPassCreateInfo rci = {.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&attachment,
        .subpassCount=1,.pSubpasses=&sub,.dependencyCount=2,.pDependencies=deps};
    VkRenderPass pass; CHECK(vkCreateRenderPass(dev, &rci, NULL, &pass));
    VkFramebufferCreateInfo fci = {.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=pass,.attachmentCount=1,
        .pAttachments=&view,.width=2,.height=1,.layers=1};
    VkFramebuffer fb; CHECK(vkCreateFramebuffer(dev, &fci, NULL, &fb));
    VkPushConstantRange range = {VK_SHADER_STAGE_FRAGMENT_BIT,0,16};
    VkPipelineLayoutCreateInfo lci = {.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.pushConstantRangeCount=1,.pPushConstantRanges=&range};
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(dev, &lci, NULL, &layout));
    VkShaderModule vs = module(argv[1]), fs = module(argv[2]);
    VkPipelineShaderStageCreateInfo stages[] = {
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vs,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fs,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vert = {.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo viewport = {.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
    VkPipelineRasterizationStateCreateInfo raster = {.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=1};
    VkPipelineColorBlendAttachmentState blend = {.colorWriteMask=15};
    VkPipelineColorBlendStateCreateInfo blends = {.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&blend};
    VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=2,.pDynamicStates=states};
    VkGraphicsPipelineCreateInfo pci = {.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .flags=atoi(argv[3])?VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT:0,.stageCount=2,.pStages=stages,
        .pVertexInputState=&vert,.pInputAssemblyState=&ia,.pViewportState=&viewport,.pRasterizationState=&raster,
        .pMultisampleState=&ms,.pColorBlendState=&blends,.pDynamicState=&dynamic,.layout=layout,.renderPass=pass};
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    VkPipeline pipeline; CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pci, NULL, &pipeline));
    clock_gettime(CLOCK_MONOTONIC, &b);
    fprintf(stderr, "pipeline %.3f s\n", (b.tv_sec-a.tv_sec)+(b.tv_nsec-a.tv_nsec)/1e9);
    VkCommandPoolCreateInfo cpci = {.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};
    VkCommandPool pool; CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
    VkCommandBufferAllocateInfo cai = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,
        .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(dev, &cai, &cmd));
    VkCommandBufferBeginInfo begin = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd, &begin));
    VkClearValue clear = {0};
    VkRenderPassBeginInfo rb = {.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,.framebuffer=fb,
        .renderArea={{0,0},{2,1}},.clearValueCount=1,.pClearValues=&clear};
    vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE); vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    for (int i = 0; i < 2; i++) {
        uint32_t selector = argc == 5 ? (uint32_t)strtoull(argv[4], NULL, 0) + (uint32_t)i : (uint32_t)(i?17:0);
        uint32_t data[] = {selector,3,0,0};
        VkViewport vp = {(float)i,0,1,1,0,1}; VkRect2D sc = {{i,0},{1,1}};
        vkCmdSetViewport(cmd, 0, 1, &vp); vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, data); vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(cmd);
    VkBufferImageCopy copy = {.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={2,1,1}};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    VkMemoryBarrier barrier = {.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit = {.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
    CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
    for (int i = 0; i < 8; i++) printf("%08x%c", pixels[i],i==7?'\n':' ');
    vkDestroyCommandPool(dev,pool,NULL); vkDestroyPipeline(dev,pipeline,NULL);
    vkDestroyShaderModule(dev,vs,NULL); vkDestroyShaderModule(dev,fs,NULL); vkDestroyPipelineLayout(dev,layout,NULL);
    vkDestroyFramebuffer(dev,fb,NULL); vkDestroyRenderPass(dev,pass,NULL); vkDestroyImageView(dev,view,NULL);
    vkDestroyImage(dev,image,NULL); vkFreeMemory(dev,imem,NULL); vkUnmapMemory(dev,bmem);
    vkDestroyBuffer(dev,buffer,NULL); vkFreeMemory(dev,bmem,NULL); vkDestroyDevice(dev,NULL); vkDestroyInstance(inst,NULL);
}
