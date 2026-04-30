本文档为initramfs的数据结构，数据分布规范文档
首先initramfs在内存中的交付格式为一段连续的物理地址区间，现规定起始基址为base,下面的讨论都建立在这个base上
1.数据结构定义：initramfs由三段组成[头部][元数据][数据段]
2.头部结构定义：
    起始于base
    struct initramfs_header{
    uint64_t magic;
    uint64_t version;
    uint64_t flags;
    uint64_t file_entry_count;
    uint64_t metadata_seg_offset;
    uint64_t metadata_seg_size;
    uint64_t data_seg_offset;
    uint64_t data_seg_size;
};
    定义这个为header指针
3.元数据结构定义：
    元数据段的起始位置为
    meta_base=base + header->metadata_seg_offset;从这个段开始的就是文件表
    格式为
    struct file_entry{
    uint64_t file_size;
    uint64_t file_path_in_metadata_offset;
    uint64_t file_in_dataseg_offset;
};
其中这个数组的项数为header->file_entry_count个
    对于任意文件表的路径，设这个文件的项为file[a],则路径的文本为
    char* path=meta_base + file[a].file_path_in_metadata_offset;
4.数据段定义
    数据段的起始位置为
    data_base=base + header->data_seg_offset;
    还是file[a],其绝对物理基址为
    uint64_t file_base=data_base + file[a].file_in_dataseg_offset;
    占领[file_base,file_base+file[a].file_size)的物理地址区间