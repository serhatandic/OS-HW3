#include "ext2fs.h"
#include "identifier.h"
#include "ext2fs_print.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <memory>
#include <unistd.h> 
#include <fcntl.h> 
#include <sys/types.h>
#include <sys/stat.h> 
#include "identifier.h"

#define EXT2_BLOCK_SIZE(sb) (1024 << (sb)->log_block_size)

using namespace std;

// Declarations of functions for bitmap and pointer recovery
void read_inode(int fd, int inode_index, struct ext2_inode *inode);
off_t calculate_inode_table_start(int fd, int block_group);
void fetch_superblock(int fd, struct ext2_super_block *sb);
void print_bitmap(char *bitmap, int size);

void recover_inode_bitmaps(int fd, ext2_super_block *sb);
void recover_block_bitmaps(int fd, ext2_super_block *sb);
void recover_pointers(int fd, ext2_super_block *sb);
void write_inode(int fd, int inode_index, ext2_inode *inode);
int is_block_used(int fd, int block_index, ext2_super_block *sb);
bool is_block_empty(const char *block, int size);
int check_block_pointer(int fd, uint32_t block, int block_index, int level, ext2_super_block *sb);
void set_bit_in_global_bitmap(uint32_t block_index, vector<char> &global_bitmap);
void update_global_bitmap(int fd, ext2_inode &inode, vector<char> &global_bitmap, ext2_super_block *sb);
void update_bitmap_for_indirect_blocks(int fd, uint32_t block, int level, vector<char> &global_bitmap, ext2_super_block *sb, bool is_directory);
void update_directory_entries_bitmap(int fd, uint32_t block, vector<char> &global_bitmap, ext2_super_block *sb);
int find_block_group(uint32_t block_num, ext2_super_block *sb);
void mark_metadata_blocks_used(int fd, ext2_super_block *sb, vector<char> &global_bitmap);

int main(int argc, char *argv[]) {
    // Read command line arguments
    if (argc < 3) {
        cerr << "Usage: " << argv[0] << " <image_location> <data_identifier>" << endl;
        return EXIT_FAILURE;
    }
    string image_path = argv[1];
    //unique_ptr<uint8_t[]> data_identifier(parse_identifier(argc - 2, &argv[2]));
    //uint8_t *data_identifier = parse_identifier(argc - 2, &argv[2]);

    int fd = open(image_path.c_str(), O_RDWR);
    if (fd == -1) {
        perror("Failed to open image file");
        return EXIT_FAILURE;
    }

    ext2_super_block sb;
    fetch_superblock(fd, &sb);
    std::cout << "Block size is " << EXT2_BLOCK_SIZE(&sb) << std::endl; 
    recover_inode_bitmaps(fd, &sb);
    //recover_block_bitmaps(fd, &sb);
    //recover_pointers(fd, &sb);

    close(fd);

    return EXIT_SUCCESS;
}

void recover_inode_bitmaps(int fd, ext2_super_block *sb) {
    int total_inode_bitmap_size = (sb->inode_count + 7) / 8; // Calculate total size needed for the inode bitmap
    vector<char> global_inode_bitmap(total_inode_bitmap_size, 0);

    // Pre-mark reserved inodes and special directories as used
    for (int i = 1; i <= 11; i++) { // Adjust as needed if there are more reserved inodes
        int index = (i - 1) / 8;
        int bit = (i - 1) % 8;
        global_inode_bitmap[index] |= (1 << bit);
    }

    // Populate the global inode bitmap based on the inode link counts
    for (int i = 1; i <= sb->inode_count; i++) {
        ext2_inode inode;
        read_inode(fd, i - 1, &inode); // Assuming inodes start from index 0
        if (inode.link_count > 0) {    // Check if inode is in use
            int bitmap_index = (i - 1) / 8;
            int bit_index = (i - 1) % 8;
            global_inode_bitmap[bitmap_index] |= (1 << bit_index);
        }
    }

    // Iterate over all block groups to correct the inode bitmaps
    int block_groups_count = (sb->block_count / sb->blocks_per_group) + (sb->block_count % sb->blocks_per_group ? 1 : 0);
    for (int group = 0; group < block_groups_count; group++) {
        ext2_block_group_descriptor bgd;
        lseek(fd, 1024 + EXT2_SUPER_BLOCK_SIZE + group * sizeof(bgd), SEEK_SET);
        read(fd, &bgd, sizeof(bgd));

        vector<char> inode_bitmap((sb->inodes_per_group + 7) / 8);
        lseek(fd, bgd.inode_bitmap * EXT2_UNLOG(sb->log_block_size), SEEK_SET);
        read(fd, inode_bitmap.data(), (sb->inodes_per_group + 7) / 8);

        // Correct the inode bitmap using the global inode bitmap
        for (int i = 0; i < sb->inodes_per_group && (group * sb->inodes_per_group + i) < sb->inode_count; i++) {
            int global_index = group * sb->inodes_per_group + i;
            int local_index = i;

            int byte_index = local_index / 8;
            int bit_index = local_index % 8;

            char global_bit = (global_inode_bitmap[global_index / 8] >> (global_index % 8)) & 1;
            char current_bit = (inode_bitmap[byte_index] >> bit_index) & 1;

            if (global_bit != current_bit) {
                if (global_bit == 1) {
                    inode_bitmap[byte_index] |= (1 << bit_index);  // Set the bit
                } else {
                    inode_bitmap[byte_index] &= ~(1 << bit_index); // Clear the bit
                }
            }
        }

        // Write the corrected inode bitmap back
        lseek(fd, bgd.inode_bitmap * EXT2_BLOCK_SIZE(sb), SEEK_SET);
        write(fd, inode_bitmap.data(), sb->inodes_per_group / 8);
    }
}

void recover_block_bitmaps(int fd, ext2_super_block *sb) {
    int block_groups_count = (sb->block_count / sb->blocks_per_group) + (sb->block_count % sb->blocks_per_group ? 1 : 0);
    int total_bitmap_size = (sb->block_count + 7) / 8;
    vector<char> global_bitmap(total_bitmap_size, 0);
    int total_blocks = sb->block_count;
    int block_size = EXT2_BLOCK_SIZE(sb);

    // Populate global bitmap by checking all inodes
    for (int i = 1; i <= sb->inode_count; i++) {
        ext2_inode inode;
        read_inode(fd, i, &inode);
        update_global_bitmap(fd, inode, global_bitmap, sb);
    }

    // Mark blocks that are clearly in use or have data
    for (int block = 0; block < total_blocks; block++) {
        vector<char> buffer(block_size);
        
        lseek(fd, block * block_size, SEEK_SET);
        read(fd, buffer.data(), block_size);

        // Simple check to determine if the block is empty
        if (!is_block_empty(buffer.data(), block_size)) {
            set_bit_in_global_bitmap(block, global_bitmap);
        }
    }

    mark_metadata_blocks_used(fd, sb, global_bitmap);

    // For each block group, use the global bitmap to correct the actual block bitmap
    for (int group = 0; group < block_groups_count; group++) {
        ext2_block_group_descriptor bgd;
        lseek(fd, 1024 + EXT2_SUPER_BLOCK_SIZE + (group * sizeof(bgd)), SEEK_SET);
        read(fd, &bgd, sizeof(bgd));

        vector<char> block_bitmap(sb->blocks_per_group / 8);
        lseek(fd, bgd.block_bitmap * EXT2_UNLOG(sb->log_block_size), SEEK_SET);
        read(fd, block_bitmap.data(), sb->blocks_per_group / 8);

        // Correct the actual bitmap using the global bitmap
        int start_block = group * sb->blocks_per_group;
        int end_block = start_block + sb->blocks_per_group;
        for (int i = start_block; i < end_block && i < sb->block_count; i++) {
            int global_index = i;
            int local_index = i - start_block;

            int byte_index = local_index / 8;
            int bit_index = local_index % 8;

            char global_bit = (global_bitmap[global_index / 8] >> (global_index % 8)) & 1;
            char current_bit = (block_bitmap[byte_index] >> bit_index) & 1;

            // If the global bitmap says the block should be used but it's not marked as used
            if (global_bit && !current_bit) {
                // Set the bit in the block bitmap
                block_bitmap[byte_index] |= (1 << bit_index);
            }
        }

        // Write corrected actual bitmap back
        lseek(fd, bgd.block_bitmap * EXT2_UNLOG(sb->log_block_size), SEEK_SET);
        write(fd, block_bitmap.data(), sb->blocks_per_group / 8);
    }
}

void mark_metadata_blocks_used(int fd, ext2_super_block *sb, vector<char> &global_bitmap) {
    int blocks_per_group = sb->blocks_per_group;
    
    // Other metadata for each block group
    int block_groups_count = (sb->block_count / sb->blocks_per_group) + (sb->block_count % sb->blocks_per_group ? 1 : 0);
    int block_size = EXT2_UNLOG(sb->log_block_size);
    for (int group = 0; group < block_groups_count; group++) {
        ext2_block_group_descriptor bgd;

        // Read block group descriptor
        lseek(fd, 1024 + EXT2_SUPER_BLOCK_SIZE + group * sizeof(bgd), SEEK_SET);
        read(fd, &bgd, sizeof(bgd));

        // Calculate the last block of the inode table and mark all blocks until then     
        int inodes_per_block = block_size / EXT2_INODE_SIZE;
        int inode_table_size = (sb->inodes_per_group + inodes_per_block - 1) / inodes_per_block;
        int start_block = group * blocks_per_group;
        int end_block = bgd.inode_table + inode_table_size;
        for (int i = start_block; i < end_block; i++) {
            set_bit_in_global_bitmap(i, global_bitmap);
        }
    }
}

void update_global_bitmap(int fd, ext2_inode &inode, vector<char> &global_bitmap, ext2_super_block *sb) {
    // Skip unused inodes
    int block_size = EXT2_UNLOG(sb->log_block_size);
    if (inode.mode == 0 || inode.link_count == 0) {  
        return;
    }

    int is_directory = (inode.mode & S_IFDIR) != 0;
    
    // Update bitmap for direct blocks
    for (int j = 0; j < 12; j++) {
        if (inode.direct_blocks[j] != 0) {
            //printf(" Processing direct block \n");
            set_bit_in_global_bitmap(inode.direct_blocks[j], global_bitmap);
        }
    }

    // Update bitmap for indirect blocks
    update_bitmap_for_indirect_blocks(fd, inode.single_indirect, 1, global_bitmap, sb, is_directory);
    set_bit_in_global_bitmap(inode.single_indirect, global_bitmap);

    update_bitmap_for_indirect_blocks(fd, inode.double_indirect, 2, global_bitmap, sb, is_directory);
    set_bit_in_global_bitmap(inode.double_indirect, global_bitmap);

    update_bitmap_for_indirect_blocks(fd, inode.triple_indirect, 3, global_bitmap, sb, is_directory);
    set_bit_in_global_bitmap(inode.triple_indirect, global_bitmap);
}

void update_bitmap_for_indirect_blocks(int fd, uint32_t block_index, int level, vector<char> &global_bitmap, ext2_super_block *sb, bool is_directory) {
    if (block_index == 0) {
        return;
    }

    int block_size = EXT2_UNLOG(sb->log_block_size);
    vector<uint32_t> block_pointers(block_size / sizeof(uint32_t));

    lseek(fd, block_index * block_size, SEEK_SET);
    read(fd, block_pointers.data(), block_size);

    int pointers_per_block = block_size / sizeof(uint32_t);

    if (level == 1) {
        for (int i = 0; i < pointers_per_block; i++) {
            if (block_pointers[i] != 0) {
                set_bit_in_global_bitmap(block_pointers[i], global_bitmap);
            }
        }
    } else {
        for (int i = 0; i < pointers_per_block; i++) {
            if (block_pointers[i] != 0) {
                set_bit_in_global_bitmap(block_pointers[i], global_bitmap);
                update_bitmap_for_indirect_blocks(fd, block_pointers[i], level - 1, global_bitmap, sb, is_directory);
            }
        }
    }
}

void update_directory_entries_bitmap(int fd, uint32_t block_index, vector<char> &global_bitmap, ext2_super_block *sb) {
    if (block_index == 0) {
        return;
    }

    uint32_t block_size = EXT2_UNLOG(sb->log_block_size);
    unique_ptr<char[]> block_data(new char[block_size]);

    lseek(fd, block_index * block_size, SEEK_SET);
    read(fd, block_data.get(), block_size);

    int offset = 0;
    while (offset < block_size) {
        ext2_dir_entry *entry = reinterpret_cast<ext2_dir_entry *>(block_data.get() + offset);
        if (entry->inode != 0) {
            set_bit_in_global_bitmap(entry->inode, global_bitmap); // Mark the inode as used
        }
        offset += entry->length;
    }
}

// Function to determine which block group a block belongs to
int find_block_group(uint32_t block_num, ext2_super_block *sb) {
    return block_num / sb->blocks_per_group;
}

void recover_pointers(int fd, ext2_super_block *sb) {
    // You would iterate over all inodes and check the validity of each block pointer
    for (int i = 0; i < sb->inode_count; i++) {
        ext2_inode inode;
        read_inode(fd, i, &inode);  // Assuming read_inode is a function to read an inode given its index
        // Check direct block pointers
        for (int j = 0; j < 12; j++) {
            if (inode.direct_blocks[j] == 0) {
                // Logic to determine if this should actually point to a block
                // and recover the pointer accordingly
            }
        }

        // Check indirect, double indirect, triple indirect
        // You would load the block pointed by single_indirect, if zero try to recover, then proceed to check blocks it points to

        // Write changes back to the inode in the file system
        write_inode(fd, i, &inode);  // Assuming write_inode is a function to write an inode given its index
    }
}

void write_inode(int fd, int inode_index, ext2_inode *inode) {
    ext2_super_block sb;
    fetch_superblock(fd, &sb);
    int inodes_per_group = sb.inodes_per_group;

    off_t inode_table_start = calculate_inode_table_start(fd, inode_index / inodes_per_group);
    int inode_offset = inode_index % inodes_per_group * sizeof(ext2_inode);
    lseek(fd, inode_table_start + inode_offset, SEEK_SET);
    write(fd, inode, sizeof(ext2_inode));
}

int is_block_used(int fd, int block_index, ext2_super_block *sb) {
    ext2_inode inode;
    int inodes_count = sb->inode_count;

    for (int i = 1; i <= inodes_count; i++) {
        read_inode(fd, i, &inode);

        if (inode.mode == 0 || inode.link_count == 0) {  // Skip unused inodes
            continue;
        }

        // Check direct block pointers
        for (int j = 0; j < 12; j++) {
            if (inode.direct_blocks[j] == block_index) {
                return 1;  // Block is used
            }
        }

        // Check indirect block pointers
        if (check_block_pointer(fd, inode.single_indirect, block_index, 1, sb)) {
            return 1;
        }

        // Check double indirect block pointers
        if (check_block_pointer(fd, inode.double_indirect, block_index, 2, sb)) {
            return 1;
        }

        // Check triple indirect block pointers
        if (check_block_pointer(fd, inode.triple_indirect, block_index, 3, sb)) {
            return 1;
        }
    }

    return 0;  // Block is not used
}

int check_block_pointer(int fd, uint32_t block, int block_index, int level, ext2_super_block *sb) {
    if (block == 0) {
        return 0;
    }

    uint32_t block_size = 1024 << sb->log_block_size;
    unique_ptr<uint32_t[]> block_pointers(new uint32_t[block_size]);

    lseek(fd, block * block_size, SEEK_SET);
    read(fd, block_pointers.get(), block_size);

    int pointers_per_block = block_size / sizeof(uint32_t);

    if (level == 1) {
        for (int i = 0; i < pointers_per_block; i++) {
            if (block_pointers[i] == block_index) {
                return 1;
            }
        }
    } else {
        for (int i = 0; i < pointers_per_block; i++) {
            if (check_block_pointer(fd, block_pointers[i], block_index, level - 1, sb)) {
                return 1;
            }
        }
    }

    return 0;
}

void fetch_superblock(int fd, ext2_super_block *sb) {
    lseek(fd, 1024, SEEK_SET); 
    read(fd, sb, sizeof(ext2_super_block));
}

bool is_block_empty(const char *block, int size) {
    for (int i = 0; i < size; ++i) {
        if (block[i] != 0) return false; 
    }
    return true;
}

// Function to determine which block group a block belongs to
int find_block_group(uint32_t block_num, const ext2_super_block *sb) {
    return block_num / sb->blocks_per_group;
}

void set_bit_in_global_bitmap(uint32_t block_index, vector<char> &global_bitmap) {
    int byte_index = block_index / 8;
    int bit_index = block_index % 8;
    global_bitmap[byte_index] |= (1 << bit_index);
}

void read_inode(int fd, int inode_index, ext2_inode *inode) {
    ext2_super_block sb;
    fetch_superblock(fd, &sb);

    int inodes_per_group = sb.inodes_per_group;
    off_t inode_table_start = calculate_inode_table_start(fd, inode_index / inodes_per_group);
    int inode_offset = (inode_index % inodes_per_group) * EXT2_INODE_SIZE;
    lseek(fd, inode_table_start + inode_offset, SEEK_SET);
    read(fd, inode, sizeof(ext2_inode));
}

// Calculate the start of the inode table for a given block group
off_t calculate_inode_table_start(int fd, int block_group) {
    ext2_super_block sb;
    ext2_block_group_descriptor bgd;
    
    fetch_superblock(fd, &sb);

    // Calculate block group descriptor table start, immediately following the superblock
    int bg_table_start = 1024 + EXT2_SUPER_BLOCK_SIZE;
    lseek(fd, bg_table_start + block_group * sizeof(bgd), SEEK_SET);
    read(fd, &bgd, sizeof(bgd));

    int block_size = EXT2_UNLOG(sb.log_block_size);  // Calculate block size
    return static_cast<off_t>(bgd.inode_table) * block_size;  // Return start of the inode table
}

void print_bitmap(const vector<char> &bitmap) {
    for (size_t i = 0; i < bitmap.size(); ++i) {
        // Print each byte as bits
        for (int j = 0; j < 8; ++j) {
            cout << ((bitmap[i] & (1 << j)) ? '1' : '0');
        }
        cout << " ";
    }
    cout << endl;
}