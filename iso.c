#include "ext2fs.h"
#include "identifier.h"
#include "ext2fs_print.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include "helper.h"

#define EXT2_BLOCK_SIZE(sb) (1024 << (sb)->log_block_size)

// Declarations of functions for bitmap and pointer recovery
void recover_inode_bitmaps(int fd, struct ext2_super_block *sb);
void recover_block_bitmaps(int fd, struct ext2_super_block *sb);
void recover_pointers(int fd, struct ext2_super_block *sb);
void write_inode(int fd, int inode_index, struct ext2_inode *inode);
int is_block_used(int fd, int block_index, struct ext2_super_block *sb);
int check_block_pointer(int fd, uint32_t block, int block_index, int level, struct ext2_super_block *sb);
void set_bit_in_global_bitmap(uint32_t block_index, char *global_bitmap);
void update_global_bitmap(int fd, struct ext2_inode *inode, char *global_bitmap, struct ext2_super_block *sb);
void update_bitmap_for_indirect_blocks(int fd, uint32_t block, int level, char *global_bitmap, struct ext2_super_block *sb, int is_directory);
void update_directory_entries_bitmap(int fd, uint32_t block, char *global_bitmap, struct ext2_super_block *sb);
int find_block_group(uint32_t block_num, struct ext2_super_block *sb);
void mark_metadata_blocks_used(int fd, struct ext2_super_block *sb, char *global_bitmap);

int main(int argc, char *argv[]) {
    // Read command line arguments
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <image_location> <data_identifier>\n", argv[0]);
        return EXIT_FAILURE;
    }
    char *image_path = argv[1];
    uint8_t *data_identifier = parse_identifier(argc - 2, &argv[2]);


    int fd = open(image_path, O_RDWR);
    if (fd == -1) {
        perror("Failed to open image file");
        return EXIT_FAILURE;
    }

    struct ext2_super_block sb;
    fetch_superblock(fd, &sb);

    print_super_block(&sb);
    recover_inode_bitmaps(fd, &sb);
    recover_block_bitmaps(fd, &sb);
    //recover_pointers(fd, &sb);

    close(fd);
    free(data_identifier);

    return EXIT_SUCCESS;
}

void recover_inode_bitmaps(int fd, struct ext2_super_block *sb) {
    int total_inode_bitmap_size = (sb->inode_count + 7) / 8; // Calculate total size needed for the inode bitmap
    char *global_inode_bitmap = calloc(total_inode_bitmap_size, sizeof(char));
    if (!global_inode_bitmap) {
        perror("Memory allocation for global inode bitmap failed");
        return;
    }

    // Pre-mark reserved inodes and special directories as used
    for (int i = 1; i <= 11; i++) { // Adjust as needed if there are more reserved inodes
        int index = (i - 1) / 8;
        int bit = (i - 1) % 8;
        global_inode_bitmap[index] |= (1 << bit);
    }

    // Populate the global inode bitmap based on the inode link counts
    for (int i = 1; i <= sb->inode_count; i++) {
        struct ext2_inode inode;
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
        struct ext2_block_group_descriptor bgd;
        lseek(fd, 1024 + EXT2_SUPER_BLOCK_SIZE + group * sizeof(bgd), SEEK_SET);
        read(fd, &bgd, sizeof(bgd));

        char *inode_bitmap = (char *)malloc((sb->inodes_per_group + 7) / 8);
        lseek(fd, bgd.inode_bitmap * EXT2_UNLOG(sb->log_block_size), SEEK_SET);
        read(fd, inode_bitmap, (sb->inodes_per_group + 7) / 8);

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
        write(fd, inode_bitmap, sb->inodes_per_group / 8);
        free(inode_bitmap);
    }

    free(global_inode_bitmap);
}




void recover_block_bitmaps(int fd, struct ext2_super_block *sb) {
    int block_groups_count = (sb->block_count / sb->blocks_per_group) + (sb->block_count % sb->blocks_per_group ? 1 : 0);
    int total_bitmap_size = (sb->block_count + 7) / 8; // Total size needed for the custom bitmap of all blocks
    printf("total bitmap size is %d ", total_bitmap_size);
    char *global_bitmap = calloc(total_bitmap_size, sizeof(char)); // Allocate and zero initialize global bitmap

    if (!global_bitmap) {
        perror("Memory allocation for global bitmap failed");
        return;
    }

    // Populate global bitmap by checking all inodes
    for (int i = 1; i < sb->inode_count; i++) {
        struct ext2_inode inode;
        read_inode(fd, i, &inode);
        //printf("updating using %d %d \n", i, sb->inode_count);
        update_global_bitmap(fd, &inode, global_bitmap, sb);
        // printf("update finished  \n");
    }
    mark_metadata_blocks_used(fd, sb, global_bitmap);
    
    print_bitmap(global_bitmap, total_bitmap_size);
    // For each block group, use the global bitmap to correct the actual block bitmap
    for (int group = 0; group < block_groups_count; group++) {
        struct ext2_block_group_descriptor bgd;
        lseek(fd, 1024 + EXT2_SUPER_BLOCK_SIZE + (group * sizeof(bgd)), SEEK_SET);
        read(fd, &bgd, sizeof(bgd));

        char *block_bitmap = (char *)malloc(sb->blocks_per_group / 8);
        lseek(fd, bgd.block_bitmap * EXT2_UNLOG(sb->log_block_size), SEEK_SET);
        read(fd, block_bitmap, sb->blocks_per_group / 8);

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
        write(fd, block_bitmap, sb->blocks_per_group / 8);

        free(block_bitmap);
    }
    free(global_bitmap);
}


void mark_metadata_blocks_used(int fd, struct ext2_super_block *sb, char *global_bitmap) {
    // Mark the superblock and block group descriptor table
    int blocks_per_group = sb->blocks_per_group;
    
    // Other metadata for each block group
    int block_groups_count = (sb->block_count / sb->blocks_per_group) + (sb->block_count % sb->blocks_per_group ? 1 : 0);
    int block_size = EXT2_UNLOG(sb->log_block_size);
    for (int group = 0; group < block_groups_count; group++) {
        struct ext2_block_group_descriptor bgd;

        // Read block group descriptor
        lseek(fd, 1024 + EXT2_SUPER_BLOCK_SIZE + group * sizeof(bgd), SEEK_SET);
        read(fd, &bgd, sizeof(bgd));


        // Calculate the last block of the inode table and mark all blocks until then     
        int inodes_per_block = block_size / EXT2_INODE_SIZE;
        int inode_table_size = (sb->inodes_per_group + inodes_per_block -1 ) / inodes_per_block;
        int start_block = group*blocks_per_group;
        int end_block = bgd.inode_table + inode_table_size;
        for (int i = start_block ; i < end_block; i++) {
            set_bit_in_global_bitmap(i, global_bitmap);
        }
    }
}



void update_global_bitmap(int fd, struct ext2_inode *inode, char *global_bitmap, struct ext2_super_block *sb) {
    // Skip unused inodes
    int block_size = EXT2_UNLOG(sb->log_block_size);
    int block_index = 0;
    if (inode->mode == 0 || inode->link_count == 0) {  
        return;
    }

    int is_directory = (inode->mode & S_IFDIR) != 0;
    
    // Update bitmap for direct blocks
    for (int j = 0; j < 12; j++) {
        if (inode->direct_blocks[j] != 0) {
            //printf(" Processing direct block \n");
            block_index = inode->direct_blocks[j] % block_size;
            set_bit_in_global_bitmap(block_index, global_bitmap);
            if (is_directory) {
                update_directory_entries_bitmap(fd, inode->direct_blocks[j], global_bitmap, sb);
            }
        }
    }

    // Update bitmap for indirect blocks
    update_bitmap_for_indirect_blocks(fd, inode->single_indirect, 1, global_bitmap, sb, is_directory);
    block_index = inode->single_indirect % block_size;
    set_bit_in_global_bitmap(block_index, global_bitmap);

    update_bitmap_for_indirect_blocks(fd, inode->double_indirect, 2, global_bitmap, sb, is_directory);
    block_index = inode->double_indirect % block_size;
    set_bit_in_global_bitmap(block_index, global_bitmap);

    update_bitmap_for_indirect_blocks(fd, inode->triple_indirect, 3, global_bitmap, sb, is_directory);
    block_index = inode->triple_indirect % block_size;
    set_bit_in_global_bitmap(block_index, global_bitmap);
}

// Updated function to calculate block pointers' physical addresses using the above helper functions
void update_bitmap_for_indirect_blocks(int fd, uint32_t block_index, int level, char *global_bitmap, struct ext2_super_block *sb, int is_directory) {
    //printf(" setting bits for indirect group level : %d  block index is : %d \n", level, (int)block_index);
    if (block_index == 0) {
        return;
    }

    int block_size = EXT2_UNLOG(sb->log_block_size);
    //printf("block size is %d \n", block_size);
    uint32_t *block_pointers = malloc(block_size);
    if (!block_pointers) {
        perror("Memory allocation failed");
        return;
    }

    struct ext2_block_group_descriptor bgd;
    int block_group = find_block_group(block_index, sb);
    lseek(fd, 1024 + EXT2_SUPER_BLOCK_SIZE + block_group * sizeof(bgd), SEEK_SET);
    read(fd, &bgd, sizeof(bgd));

    lseek(fd, block_index * block_size, SEEK_SET);
    read(fd, block_pointers, block_size);

    int pointers_per_block = block_size / sizeof(uint32_t);

    if (level == 1) {
        for (int i = 0; i < pointers_per_block; i++) {
            if (block_pointers[i] != 0) {
                set_bit_in_global_bitmap(block_pointers[i], global_bitmap);
                if (is_directory) {

                    update_directory_entries_bitmap(fd, block_pointers[i], global_bitmap, sb);
                }
            }
        }
    } 
    else {
        for (int i = 0; i < pointers_per_block; i++) {
            if (block_pointers[i] != 0) {
                update_bitmap_for_indirect_blocks(fd, block_pointers[i], (level - 1), global_bitmap, sb, is_directory);
            }
        }
    }
    free(block_pointers);
}

void update_directory_entries_bitmap(int fd, uint32_t block_index, char *global_bitmap, struct ext2_super_block *sb) {
    //printf(" setting bits for directory entries, block index : %d \n", block_index);
    if (block_index == 0) {
        return;
    }

    uint32_t block_size = EXT2_UNLOG(sb->log_block_size);
    char *block_data = malloc(block_size);
    if (!block_data) {
        perror("Failed to allocate memory for directory block");
        return;
    }

    // Calculate the correct block offset within the filesystem
    struct ext2_block_group_descriptor bgd;
    int block_group = find_block_group(block_index, sb);
    lseek(fd, 1024 + EXT2_SUPER_BLOCK_SIZE + block_group * sizeof(bgd), SEEK_SET);
    read(fd, &bgd, sizeof(bgd));


    lseek(fd, block_index * block_size, SEEK_SET);
    read(fd, block_data, block_size);

    int offset = 0;
    while (offset < block_size) {
        struct ext2_dir_entry *entry = (struct ext2_dir_entry *)(block_data + offset);
        if (entry->inode != 0) {
            //printf("entry i_node: %d \n", entry->inode);
            set_bit_in_global_bitmap(entry->inode, global_bitmap); // Mark the inode as used
        }
        offset += entry->length;
    }

    free(block_data);
}


// Function to determine which block group a block belongs to
int find_block_group(uint32_t block_num, struct ext2_super_block *sb) {
    return block_num / sb->blocks_per_group;
}


void set_bit_in_global_bitmap(uint32_t block_index, char *global_bitmap) {
    int block_group = block_index / 8;
    int bit_index = block_index % 8;
    //printf(" setting the bit, %d \n", block_index);
    global_bitmap[block_group] |= (1 << bit_index);
}



























void recover_pointers(int fd, struct ext2_super_block *sb) {
    // You would iterate over all inodes and check the validity of each block pointer
    for (int i = 0; i < sb->inode_count; i++) {
        struct ext2_inode inode;
        read_inode(fd, i, &inode);  // Assuming read_inode is a function to read an inode given its index
        //print_inode(&inode,i);
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



void write_inode(int fd, int inode_index, struct ext2_inode *inode) {
    struct ext2_super_block sb;
    fetch_superblock(fd, &sb);
    int inodes_per_group = sb.inodes_per_group;

    off_t inode_table_start = calculate_inode_table_start(fd, inode_index / inodes_per_group);
    int inode_offset = inode_index % inodes_per_group * sizeof(struct ext2_inode);
    lseek(fd, inode_table_start + inode_offset, SEEK_SET);
    write(fd, inode, sizeof(struct ext2_inode));
}

int is_block_used(int fd, int block_index, struct ext2_super_block *sb) {
    struct ext2_inode inode;
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

int check_block_pointer(int fd, uint32_t block, int block_index, int level, struct ext2_super_block *sb) {
    if (block == 0) {
        return 0;
    }

    uint32_t block_size = 1024 << sb->log_block_size;
    uint32_t *block_pointers = malloc(block_size);
    if (!block_pointers) {
        perror("Memory allocation failed");
        return 0;
    }

    lseek(fd, block * block_size, SEEK_SET);
    read(fd, block_pointers, block_size);

    int pointers_per_block = block_size / sizeof(uint32_t);

    if (level == 1) {
        for (int i = 0; i < pointers_per_block; i++) {
            if (block_pointers[i] == block_index) {
                free(block_pointers);
                return 1;
            }
        }
    } else {
        for (int i = 0; i < pointers_per_block; i++) {
            if (check_block_pointer(fd, block_pointers[i], block_index, level - 1, sb)) {
                free(block_pointers);
                return 1;
            }
        }
    }

    free(block_pointers);
    return 0;
}
