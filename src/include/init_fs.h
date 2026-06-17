#pragma once
#include "BlockDevice.h"
#include <cstdint>
#include "MemoryDisk.h"
#include "util/lock.h"
typedef uint64_t FileID_t;
enum inode_op_type:int{
    INODE_OP_READ=0,
    INODE_OP_WRITE=1,
    DIR_INODE_OP_ACCESS=2,
    INODE_OP_EXECUTE = 3,
};
enum access_check_result:int{
    ACCESS_PERMITTED=0,
    ACCESS_DENIED=1,
};
struct Visitor_t
{
   uint32_t uid;
   uint32_t gid;
};
struct FileOpenType
{
    uint8_t read:1;
    uint8_t write:1;
    uint8_t append:1;
};
// 定义无效的位图索引，用于返回错误状态
static constexpr uint32_t INVALID_BITMAP_INDEX = static_cast<uint32_t>(-1);
/**
 * todo:
 * inode_content_write的时候要检查自旋锁
 * 块组描述符数组的读写只有在拓展文件系统拓展的时候才需要
 * 块组内位图读写的时候要加自旋锁
 * 优化rootinode在挂载文件系统的时候就加载到类里面，并且优化相应的get_inode函数
 */
class init_fs_t {//支持卸载后向前移动，不卸载的情况下向后增加分区大小
    block_device_t_v1* phylayer;
    const uint64_t HYPER_CLUSTER_MAGIC = 0xF0F0F0F0F0F0F0F0;
    const uint32_t SUPER_CLUSTER_MAGIT = 0x01;
    // 按照依赖关系重新排列结构体定义
    struct InnerPointer {//一个分区内文件系统指导下的指针
        uint64_t cluster_index;
        uint32_t blk_offset;
    };
    MemoryDiskv1*memdiskv1_blockdevice;//内存盘模式下的文件系统读写基本上直接跳出内存盘之外解析内存手动读写内存盘
    struct BlocksgroupDescriptor;
    struct SuperCluster;
    struct Inode;
    struct FileEntryinDir;
static constexpr uint64_t LEVLE1_INDIRECT_START_CLUSTER_INDEX = 12;
static constexpr uint32_t MAX_FILE_NAME_LEN = 56;
static constexpr uint64_t DIRECT_CLUSTERS =  LEVLE1_INDIRECT_START_CLUSTER_INDEX;
static constexpr uint64_t LEVEL2_INDIRECT_START_CLUSTER_INDEX =  LEVLE1_INDIRECT_START_CLUSTER_INDEX +512;
static constexpr uint64_t LEVEL3_INDIRECT_START_CLUSTER_INDEX =  LEVEL2_INDIRECT_START_CLUSTER_INDEX +512*512;
static constexpr uint64_t LEVEL4_INDIRECT_START_CLUSTER_INDEX =  LEVEL3_INDIRECT_START_CLUSTER_INDEX +512*512*512;
static constexpr uint32_t TRIPLE_INDIRECT_CLUSTERS=512*512*512;
static constexpr uint64_t HYPER_CLUSTER_INDEX=0;
static constexpr uint64_t CLUSTER_DEFAULT_SIZE = 4096;
static constexpr uint64_t DEFAULT_BLOCKS_GROUP_MAX_CLUSTER = 8 * 4096;
static constexpr uint16_t DIR_TYPE = 1;
static constexpr uint16_t FILE_TYPE = 0;
static constexpr uint16_t INDEX_TABLE_TYPE = 0;


// Bitmap type constants
static constexpr uint32_t INODE_BITMAP = 0;
static constexpr uint32_t CLUSTER_BITMAP = 1;
static constexpr uint32_t FILE_PATH_MAX_LEN=8192;
    struct HyperCluster {
        uint64_t magic;                      // 魔数
        uint32_t version;                    // 版本号，暂时为0
        uint32_t block_size;                 // 块大小，必须为2的次幂
        uint16_t cluster_block_count;        // 每个簇包含的块数，2的次幂
        uint16_t hyper_cluster_size;         // HyperCluster结构体大小
        uint32_t cluster_size;               // 簇大小
        uint64_t total_clusters;             // 总簇数
        uint64_t total_blocks;               // 总块数
        uint64_t total_blocks_group_max;
        uint64_t total_blocks_group_valid;
        uint64_t block_group_descriptor_first_cluster; // 块组描述符数组起始簇索引
        uint64_t block_groups_array_size_in_clusters;  // 块组数组占用的簇数
        uint32_t root_block_group_index;     // 根目录所在块组索引
        uint32_t root_directory_inode_index; // 根目录inode索引
        uint32_t max_block_group_size_in_clusters;     // 块组大小上限(以簇为单位)
        uint16_t super_cluster_size;         // SuperCluster结构体大小
        uint16_t inode_size;                 // Inode大小
        uint16_t block_group_descriptor_size; // 块组描述符大小
        uint16_t fileEntryinDir_size;
    } __attribute__((packed));
    
    struct BlocksgroupDescriptor {
        uint64_t first_cluster_index;        // 块组第一个簇索引，同时指向块组的SuperCluster
        uint32_t cluster_count;              // 块组包含的簇数
        uint32_t flags;                      // 块组标志
    } __attribute__((packed));
    
    struct SuperCluster {
        uint64_t magic;                      // 魔数
        uint64_t first_cluster_index;        // 第一个簇索引
        uint32_t version;                    // 版本号
        uint32_t cluster_count;              // 簇数量
        uint32_t clusters_bitmap_first_cluster;      // 簇位图的第一个簇索引
        uint32_t clusters_bitmap_cluster_count;              // 簇位图占用的总共簇数
        uint32_t inodes_count_max;
        uint32_t inodes_array_first_cluster;         // inode数组的第一个簇索引
        uint32_t inodes_bitmap_first_cluster;        // inode位图的第一个簇索引
        uint32_t inodes_array_cluster_count;          // inode数组占用的簇数
        uint32_t inodes_bitmap_cluster_count;         // inode位图占用的簇数
    } __attribute__((packed));
    
    struct file_flags {
        uint64_t dir_or_normal : 8;          // 0: 普通文件, 1: 目录
        uint64_t user_access : 3;            // 用户权限 (0-7)
        uint64_t group_access : 3;           // 组权限 (0-7)
        uint64_t other_access : 3;           // 其他权限 (0-7)
        uint64_t extents_or_indextable : 1;  // 0: 索引表, 1: extents
        uint64_t is_root_inode : 1;        // 是否为根目录inode
    } __attribute__((packed));
    
    struct file_index_table {
        uint64_t direct_pointers[12];        // 直接指针数组
        uint64_t single_indirect_pointer;    // 一级间接指针
        uint64_t double_indirect_pointer;    // 二级间接指针
        uint64_t triple_indirect_pointer;    // 三级间接指针
    } __attribute__((packed));
    
    struct extents_metainf {
        uint64_t first_cluster_index_of_extents_array;        // 第一个簇索引
        uint32_t entries_count;              // extent条目数量
    } __attribute__((packed));
    
    struct FileExtentsEntry_t {
        uint64_t first_cluster_phyindex;
        uint32_t length_in_clusters;
    } __attribute__((packed));
    
    union data_descript {
        file_index_table index_table;
        extents_metainf extents;
    };
    
    struct alignas(256) Inode 
    {
        uint32_t uid;                        // 用户ID
        uint32_t gid;                        // 组ID
        uint32_t parent_dir_inode_index;    // 父目录inode索引
        uint32_t parent_dir_block_group_index;          // 块组索引
        uint64_t file_size;                  // 文件大小
        file_flags flags;                    // 文件标志 
        uint64_t creation_time;              // 创建时间(秒)
        uint64_t last_modification_time;     // 最后修改时间(秒)
        uint64_t last_access_time;            // 最后访问时间(秒)     
        data_descript data_desc;             // 数据描述符
        spinrwlock_cpp_t spinwlock;
        trylock_cpp_t trylock;
    };
    Inode root_inode;
    //传给文件系统的文件路径中.是当前目录，..是上级目录
    //若是根目录则报错，因为可能根据虚拟文件系统层，根目录的父目录在其它文件系统
    /**
 * 根据状态机思想以inode为主附带一些其它变量构造一个类解析路径
 */
    class FilePathAnalyzer{
    public:
    static constexpr int START=0;
    static constexpr int IN_DIR=1;
    static constexpr int IN_FILE=2;
    static constexpr int END=3;
    static constexpr int ERROR_STATE=4;
    static constexpr int ROOT_DIR_NEED_OTHER_FILE_SYSTEM=5;
    static constexpr int ACCESS_DENIED=6;
protected:
        Inode current_inode;
        int state;
        Visitor_t visitor;
        char*path;
        uint32_t pathname_length;
        uint32_t pathname_scanner;//每次扫描完要移动到下一个目录名/文件名开始的位置
        uint32_t current_dir_block_group_index;
        uint32_t current_dir_inode_index;
        public:
        FilePathAnalyzer(Inode root_inode,char*path,uint32_t pathname_length,Visitor_t visitor);
        int the_next(init_fs_t *fs_instance);
        uint32_t get_current_inode_index();
        uint32_t get_current_block_group_index();
        int get_state();
        Inode get_current_inode();
    };
    friend  class FilePathAnalyzer;
    struct FileEntryinDir {
        uint8_t filename[MAX_FILE_NAME_LEN];                // 文件名，使用uint8_t避免平台相关的符号扩展问题
        //显然 最多55个字节
        uint32_t inode_index;                // inode索引
        uint32_t block_group_index;          // 块组索引
    } __attribute__((packed));  
    class FileExtents_in_clusterScanner
    {
        FileExtentsEntry_t* extents_array;
        uint32_t extents_count;
        uint32_t extents_entry_scanner;
        uint32_t skipped_clusters_count;
        uint32_t in_entry_offset;
        public:
        int the_next();
        FileExtents_in_clusterScanner(FileExtentsEntry_t* extents_array,uint32_t extents_count);
        uint32_t convert_to_logical_cluster_index();
        uint64_t get_phyclsidx();
    };
    struct idx_tb_lclsidx_scanner
    {
        /* data */
    };
    
    class idxtbmode_file_Iteration
    { 
        public:
        static constexpr int START=0;
        static constexpr int MID=1;
        static constexpr int END_CLS=2;
        static constexpr int ERROR_STATE=3;
        static constexpr int END=4;
        protected: Inode file_inode;
        uint16_t cls_blk_count;
        uint32_t blk_size;
        uint32_t cls_size;
        uint64_t stream_base_offset;
        uint8_t*buffer;
        uint64_t offset_ptr;
        uint64_t left_bytes_to_write;//迭代的时候不调用
        uint64_t start_lcls;
        uint32_t start_lcls_startvytes_offset;
        uint32_t start_lcls_size_to_write;
        uint64_t midbase_lcls;
        uint64_t midcls_count;
        uint64_t midcls_scanner;//从0到midcls_count-1
        uint64_t end_lcls;
        uint64_t end_lcls_size_to_write;
        int state;
        public:
        idxtbmode_file_Iteration(
            Inode file_inode,
            uint8_t*buffer,
            uint64_t stream_base_offset,
            uint16_t cls_blk_count,
            uint32_t blk_size,
            uint64_t size_to_write
        );
        int wnext(init_fs_t*fs_instance);
        int rnext(init_fs_t*fs_instance);
    };
    
    friend class idxtbmode_file_Iteration;   
    class extents_rw_iterator {
public:
    /** \brief 迭代器状态枚举 */
    enum State { START, MID, END_ENTRY, END, ERROR_STATE };
//迭代器一次处理extents数组的一个项
private:
    init_fs_t* fs;                ///< 文件系统上下文指针
    Inode file_inode;             ///< 目标文件的inode信息
    uint8_t* buffer;              ///< 数据缓冲区指针（读写操作的数据源/目标）
    uint64_t stream_offset;       ///< 逻辑流中的起始偏移量
    uint64_t left_bytes_to_write; ///< 剩余待处理字节数
    uint64_t cluster_size;        ///< 文件系统簇大小
    uint64_t accumlate_skipped_bytes; ///< 累计跳过的字节数

    /** \brief 预解析的extent数组（构造时加载）
     *  用于快速映射逻辑偏移到物理簇位置
     *  \note 非memdiskv1模式下需手动释放
     */
    FileExtentsEntry_t* extents = nullptr;
    uint32_t extent_count = 0;    ///< 总extent数量
    uint32_t extent_idx = 0;      ///< 当前处理的extent索引

    /** \brief 当前extent处理状态 */
    uint64_t clusters_written_in_current_extent = 0;
    State state;

    /** \brief 起始段处理参数 */
    uint32_t start_inarray_idx;
    uint64_t start_seg_offset;
    uint64_t start_seg_bytes_to;

    /** \brief 中间段批量处理参数 */
    uint32_t mid_array_count;
    uint32_t mid_array_baseidx;
    uint32_t mid_array_scanner;

    /** \brief 结束段处理参数 */
    uint32_t end_array_idx;
    uint64_t endseg_to_write_bytes;
    uint64_t buffer_offset = 0;   ///< 缓冲区当前偏移量

public:
    /**
     * @brief 构造函数，初始化迭代器状态
     * @param fs 文件系统上下文指针
     * @param inode 目标文件的inode
     * @param buffer 数据缓冲区指针
     * @param stream_offset 逻辑流起始偏移量
     * @param size 需处理的总字节数
     */
    extents_rw_iterator(init_fs_t* fs, Inode inode, uint8_t* buffer,
                           uint64_t stream_offset, uint64_t size);

    /**
     * @brief 析构函数，释放预加载的extent数组
     * @note 仅在非memdiskv1模式下释放资源
     */
    ~extents_rw_iterator() { if (!fs->is_memdiskv1) delete[] extents; }

    /**
     * @brief 推进写操作到下一个状态
     * @return 当前状态，调用方应循环直到END或ERROR_STATE
     */
    State wnext();

    /**
     * @brief 推进读操作到下一个状态
     * @return 当前状态，调用方应循环直到END或ERROR_STATE
     */
    State rnext();

    /**
     * @brief 获取当前迭代器状态
     * @return 当前状态枚举值
     */
    State get_state();
};
        //此函数返回void*指针，指针处获得了原始的对应块组的对应种类的位图的原始数据
    void* get_bitmap_base(
        uint64_t block_group_index,
        uint32_t bitmap_type
    );
    
    uint32_t search_avaliable_inode_bitmap_bit(//未完成所有特性
        uint64_t&block_group_index//传入0则扫描所有块组，非0则只在特定块组中搜索
    );
    uint32_t search_avaliable_cluster_bitmap_bit(//未完成所有特性
        uint64_t&block_group_index//传入0则扫描所有块组，非0则只在特定块组中搜索
    );
    /**
     * @brief 搜索指定块组中连续的可用的簇位图位
     * 
     * @param aquired_avaliable_clusters_count 获取的簇数
     * @param result_base 获取的起始簇索引
     * @param block_group_index 块组索引
     * @return int 0: 成功, 非0: 失败

     */
    int search_avaliable_cluster_bitmap_bits(
        uint64_t aquired_avaliable_clusters_count,
        uint64_t&result_base,
        uint64_t&block_group_index
    );
    int FileExtentsEntry_merger(FileExtentsEntry_t *old_buff, FileExtentsEntry_t *new_buff, uint64_t &entryies_count);
    SuperCluster*get_supercluster(uint32_t block_group_index);
    int clusters_bitmap_alloc(uint64_t alloc_clusters_count, FileExtentsEntry_t*&extents_entry, uint64_t &entry_count);
    int set_inode_bitmap_bit(
        uint64_t block_group_index,
        uint64_t inode_index,
        bool value);
    int set_inode_bitmap_bits(
        uint64_t block_group_index,
        uint64_t base_index,
        uint64_t bit_count,
        bool value
    );
    int logical_offset_toindex(
            FileExtentsEntry_t* extents_array,
            uint64_t extents_count,
            uint64_t logical_offset ,
            uint64_t &result_index
    );
    bool get_inode_bitmap_bit(
        uint64_t block_group_index,
        uint64_t inode_index,
        int &status
    );
    int set_cluster_bitmap_bit(
        uint64_t block_group_index,
        uint64_t cluster_idx,
        bool value
    );
    int set_cluster_bitmap_bits(
        uint64_t block_group_index,
        uint64_t base_index,
        uint64_t bit_count,
        bool value
    );
    /*之前的接口是在特定块组的位图下的接口，需要在特定块组特定偏移量以及范围设置
    全局接口是在全局位图下的接口，不需要知道块组，全局的簇的索引解析出对应块组，然后
    使用对应块组的接口去设置
    */
        int global_set_cluster_bitmap_bit(
        uint64_t cluster_idx,
        bool value
    );
    int global_set_cluster_bitmap_bits(
        uint64_t base_index,
        uint64_t bit_count,
        bool value
    );
    bool get_cluster_bitmap_bit(
        uint64_t block_group_index,
        uint64_t cluster_idx,
        int &status
    );
    /**
     *内部接口处理inode大小改变，
     *会根据inode指向的文件大小的改变去修改相应bitmap
     */
    int resize_inode(Inode& the_inode, uint64_t new_size);//未完成所有特性
    int Increase_inode_allocated_clusters(Inode& the_inode, uint64_t Increase_clusters_count);
    int idxtbmode_set_inode_lcluster_phyclsidx(Inode& the_inode, uint64_t lcluster_index, uint64_t phyclsidx);
    int Decrease_inode_allocated_clusters(Inode& the_inode, uint64_t Decrease_clusters_count);//未完成所有特性
    int idxtbmod_delete_lcluster(Inode& the_inode, uint64_t lcluster_index);
    int write_inode(
        uint64_t block_group_index,
        uint64_t inode_index,
        Inode* inode
    );  
    int get_inode(
        uint64_t block_group_index,
        uint64_t inode_index,
        Inode& inode
    );
    int inode_content_write(//从特定偏移量上覆写，若超出大小会报错
        Inode the_inode,
        uint64_t stream_base_offset,
        uint64_t data_stream_size,
        uint8_t*buffer
    );
    int inode_content_read(//从特定偏移量上读取，若超出大小会报错
        Inode the_inode,
        uint64_t stream_base_offset,
        uint64_t size,
        uint8_t*buffer
    );
    int filecluster_to_fscluster_in_idxtb(
        Inode the_inode,
        uint64_t file_lcluster_index,
        uint64_t&phyidx
    );
    int filecluster_to_fscluster_in_extents(
        Inode the_inode,
        uint64_t file_cluster_index,
        uint64_t&cluster_index,
        FileExtentsEntry_t& extents_entry
    );
    int dir_content_search_by_name(
        Inode be_searched_dir_inode,
        char* filename,
        FileEntryinDir& result_entry    
    );
    int dir_content_append(
        Inode&dir_inode,
        FileEntryinDir append_entry    
    );
    int dir_content_delete_by_name(
        Inode&dir_inode,
        FileEntryinDir&del_entry
    );
    access_check_result access_check(
        Visitor_t visitor,
        Inode the_inode,
        inode_op_type op_type
    );
    int path_analyze(char*path,Inode& inode,Visitor_t visitor);
    HyperCluster*fs_metainf;
    SuperCluster*SuperClusterArray;
    bool is_valid;
    bool is_memdiskv1;
    struct FopenFlags{
        uint16_t is_write:1;
        uint16_t is_read:1;
        uint16_t is_force:1;//突破尝试写锁还是要建立写句柄
        uint16_t is_append_mode:1;//追加模式，所有写入时都从文件末尾开始写入，但是可以移动文件指针读取前面的内容
    };
    FopenFlags read_only_falgs={
1,0,0,0
    };
    FopenFlags read_write_falgs={
1,1,0,0
    };

    FopenFlags read_write_force_falgs={
1,1,1,0
    };
    FopenFlags read_write_append_falgs={
1,1,0,1
    };
    struct init_fs_opened_file_entry {
        Inode inode;
        uint64_t offset_ptr;
        uint32_t inode_idx;
        uint32_t block_group_index;
        FopenFlags file_flags;
        uint8_t is_valid_entry;
    };
    init_fs_opened_file_entry *opened_file_table;
    uint32_t opened_file_count;
    uint32_t opened_file_array_max_count;
    int opened_file_entry_search_by_id(FileID_t in_fs_id,init_fs_opened_file_entry& result_entry);
    int opened_file_entry_set_by_id(FileID_t in_fs_id,init_fs_opened_file_entry& result_entry);
    int opened_file_entry_disable_by_id(FileID_t in_fs_id);
    int opened_file_entry_search_by_bgidx_and_inodeidx(
        uint32_t bgidx,
        uint32_t inode_idx,
        init_fs_opened_file_entry& result_entry
    );
    int opened_file_entry_alloc_a_new_entry(init_fs_opened_file_entry&new_entry,FileID_t&alloced_entry);
public: 

    init_fs_t(block_device_t_v1* phylayer);
    int Mkfs();//格式化
    int CreateFile(Visitor_t executor,char* relative_path);
    int DeleteFile(Visitor_t executor,char* relative_path);
    int mvFile(Visitor_t executor, char* oldname, char* newname);
    int cpFile(Visitor_t executor, char* oldname, char* newname);
    int CreateDir(Visitor_t executor, char* relative_path);
    int DeleteDir(Visitor_t executor, char* relative_path);//删除目录,必须是空目录
    int WriteFile(FileID_t in_fs_id, void* src, uint64_t size);
    int ReadFile(FileID_t in_fs_id, void* dest, uint64_t size);
    int OpenFile(Visitor_t executor, char* relative_path, FileID_t& in_fs_id,FopenFlags flags);
    int SeekFile(FileID_t in_fs_id, uint64_t offset);
    /**
     * 这是个测试四个增删函数的函数，以及其它相关用户空间接口的函数，纯用户空间函数，不在内核态编译，可以使用各种用户态工具
     */
    int Create_del_and_Inner_surface_test();
    int CloseFile(FileID_t in_fs_id);
    ~init_fs_t();
};