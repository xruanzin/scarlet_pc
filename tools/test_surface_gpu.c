// Numerical Vulkan execution of the production translator's SUST / SURED shaders, without game files.
// Mode 0: RGBA8 typeless store; mode 1: R32_UINT typeless store; mode 2: R32_UINT atomic add.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    fprintf(stderr, "%s: %d\n", #x, r_); exit(2); } } while (0)
static VkDevice dev;
static VkPhysicalDevice phys;

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties p;
    vkGetPhysicalDeviceMemoryProperties(phys, &p);
    for (uint32_t i = 0; i < p.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags) return i;
    assert(!"no suitable memory type"); return 0;
}

static void *buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buf, VkDeviceMemory *mem) {
    VkBufferCreateInfo ci = {.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=size,.usage=usage};
    CHECK(vkCreateBuffer(dev, &ci, NULL, buf));
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(dev, *buf, &r);
    VkMemoryAllocateInfo ai = {.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=r.size,
        .memoryTypeIndex=memory_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    CHECK(vkAllocateMemory(dev, &ai, NULL, mem)); CHECK(vkBindBufferMemory(dev, *buf, *mem, 0));
    void *p; CHECK(vkMapMemory(dev, *mem, 0, VK_WHOLE_SIZE, 0, &p)); memset(p, 0, (size_t)size); return p;
}

static void image(VkFormat format, VkImage *img, VkDeviceMemory *mem, VkImageView *view) {
    VkImageCreateInfo ci = {.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
        .format=format,.extent={4,1,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    CHECK(vkCreateImage(dev, &ci, NULL, img));
    VkMemoryRequirements r; vkGetImageMemoryRequirements(dev, *img, &r);
    VkMemoryAllocateInfo ai = {.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=r.size,
        .memoryTypeIndex=memory_type(r.memoryTypeBits, 0)};
    CHECK(vkAllocateMemory(dev, &ai, NULL, mem)); CHECK(vkBindImageMemory(dev, *img, *mem, 0));
    VkImageViewCreateInfo vi = {.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=*img,
        .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=format,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    CHECK(vkCreateImageView(dev, &vi, NULL, view));
}

int main(int argc, char **argv) {
    assert(argc == 3 || argc == 4); int mode = atoi(argv[2]);
    VkApplicationInfo app = {.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkInstance inst; CHECK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t count = 1; CHECK(vkEnumeratePhysicalDevices(inst, &count, &phys)); assert(count);
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
    VkQueueFamilyProperties *families = calloc(nq, sizeof *families);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, families);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; break; }
    free(families); assert(family != UINT32_MAX);
    float priority = 1;
    VkDeviceQueueCreateInfo qi = {.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,
        .queueCount=1,.pQueuePriorities=&priority};
    VkPhysicalDeviceDescriptorIndexingFeatures indexing = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,
        .shaderStorageImageArrayNonUniformIndexing=VK_TRUE};
    VkPhysicalDeviceFeatures features = {.shaderInt64=VK_TRUE,.shaderStorageImageWriteWithoutFormat=VK_TRUE,
        .shaderStorageImageArrayDynamicIndexing=VK_TRUE};
    VkDeviceCreateInfo dci = {.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&indexing,.queueCreateInfoCount=1,
        .pQueueCreateInfos=&qi,.pEnabledFeatures=&features};
    CHECK(vkCreateDevice(phys, &dci, NULL, &dev)); VkQueue queue; vkGetDeviceQueue(dev, family, 0, &queue);
    VkBuffer map_buf, output_buf; VkDeviceMemory map_mem, output_mem;
    uint32_t *map = buffer(32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &map_buf, &map_mem);
    uint32_t *output = buffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &output_buf, &output_mem);
    for (int i = 0; i < 8; i++) map[i] = UINT32_MAX;
    map[mode ? 7 : 6] = 0;  // TIC 3 -> first float or uint image slot
    VkImage images[2]; VkDeviceMemory memories[2]; VkImageView views[2];
    image(VK_FORMAT_R8G8B8A8_UNORM, &images[0], &memories[0], &views[0]);
    image(VK_FORMAT_R32_UINT, &images[1], &memories[1], &views[1]);

    VkDescriptorSetLayoutBinding bindings[3] = {
        {120,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},
        {121,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,16,VK_SHADER_STAGE_COMPUTE_BIT,NULL},
        {122,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,16,VK_SHADER_STAGE_COMPUTE_BIT,NULL}};
    VkDescriptorSetLayoutCreateInfo lci = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=3,.pBindings=bindings};
    VkDescriptorSetLayout layout; CHECK(vkCreateDescriptorSetLayout(dev, &lci, NULL, &layout));
    VkPipelineLayoutCreateInfo pci = {.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&layout};
    VkPipelineLayout pl; CHECK(vkCreatePipelineLayout(dev, &pci, NULL, &pl));
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,32}};
    VkDescriptorPoolCreateInfo dpi = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=2,.pPoolSizes=sizes};
    VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(dev, &dpi, NULL, &pool));
    VkDescriptorSetAllocateInfo dai = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,
        .descriptorSetCount=1,.pSetLayouts=&layout};
    VkDescriptorSet ds; CHECK(vkAllocateDescriptorSets(dev, &dai, &ds));
    VkDescriptorBufferInfo bi = {map_buf,0,32};
    VkDescriptorImageInfo ii[2][16];
    for (int i = 0; i < 2; i++) for (int j = 0; j < 16; j++)
        ii[i][j] = (VkDescriptorImageInfo){VK_NULL_HANDLE,views[i],VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[3] = {{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ds,.dstBinding=120,
        .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&bi}};
    for (int i = 1; i < 3; i++) writes[i] = (VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet=ds,.dstBinding=(uint32_t)(120+i),.descriptorCount=16,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,.pImageInfo=ii[i-1]};
    vkUpdateDescriptorSets(dev, 3, writes, 0, NULL);
    FILE *f = fopen(argv[1], "rb"); assert(f); fseek(f, 0, SEEK_END); long length = ftell(f); rewind(f); assert(length > 0);
    uint32_t *code = malloc((size_t)length); assert(fread(code, 1, (size_t)length, f) == (size_t)length); fclose(f);
    VkShaderModuleCreateInfo sci = {.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)length,.pCode=code};
    VkShaderModule shader; CHECK(vkCreateShaderModule(dev, &sci, NULL, &shader)); free(code);
    VkComputePipelineCreateInfo cpi = {.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.layout=pl,
        .flags=argc == 4 ? VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT : 0,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=shader,.pName="main"}};
    VkPipeline pipe; CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));
    VkCommandPoolCreateInfo cpci = {.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};
    VkCommandPool cp; CHECK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
    VkCommandBufferAllocateInfo cai = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp,
        .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(dev, &cai, &cmd));
    VkCommandBufferBeginInfo begin = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd, &begin));
    VkImageMemoryBarrier barriers[2];
    for (int i = 0; i < 2; i++) barriers[i] = (VkImageMemoryBarrier){.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=images[i],.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,NULL,0,NULL,2,barriers);
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkClearColorValue clear = {{0,0,0,0}}; vkCmdClearColorImage(cmd, images[0], VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &all);
    clear.uint32[0] = 11; vkCmdClearColorImage(cmd, images[1], VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &all);
    VkMemoryBarrier barrier = {.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,1,&barrier,0,NULL,0,NULL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0,1,&ds,0,NULL); vkCmdDispatch(cmd, 1,1,1);
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0,1,&barrier,0,NULL,0,NULL);
    VkBufferImageCopy region = {.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={4,1,1}};
    vkCmdCopyImageToBuffer(cmd, images[mode ? 1 : 0], VK_IMAGE_LAYOUT_GENERAL, output_buf, 1, &region);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,1,&barrier,0,NULL,0,NULL);
    CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit = {.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
    CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
    for (int i = 0; i < 4; i++) printf("%08x%c", output[i], i == 3 ? '\n' : ' ');
    vkDestroyCommandPool(dev, cp, NULL); vkDestroyPipeline(dev, pipe, NULL); vkDestroyShaderModule(dev, shader, NULL);
    vkDestroyDescriptorPool(dev, pool, NULL); vkDestroyPipelineLayout(dev, pl, NULL); vkDestroyDescriptorSetLayout(dev, layout, NULL);
    for (int i = 0; i < 2; i++) { vkDestroyImageView(dev, views[i], NULL); vkDestroyImage(dev, images[i], NULL); vkFreeMemory(dev, memories[i], NULL); }
    vkUnmapMemory(dev, map_mem); vkUnmapMemory(dev, output_mem);
    vkDestroyBuffer(dev, map_buf, NULL); vkDestroyBuffer(dev, output_buf, NULL);
    vkFreeMemory(dev, map_mem, NULL); vkFreeMemory(dev, output_mem, NULL);
    vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL);
}
