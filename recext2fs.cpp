#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstring>
#include "ext2fs.h"

#define BLOCK_SIZE 1024 // Example block size; this should be read from the superblock

void read_superblock(std::ifstream &fs, ext2_super_block &sb) {
    fs.seekg(1024, std::ios::beg); // Superblock starts at byte 1024
    fs.read(reinterpret_cast<char*>(&sb), sizeof(ext2_super_block));
}

void read_block_group_descriptor(std::ifstream &fs, int group, ext2_block_group_descriptor &bgd, int block_size) {
    int bgd_offset = 1024 + block_size; // BGD table follows superblock
    fs.seekg(bgd_offset + group * sizeof(ext2_block_group_descriptor), std::ios::beg);
    fs.read(reinterpret_cast<char*>(&bgd), sizeof(ext2_block_group_descriptor));
}

void read_bitmap(std::ifstream &fs, std::vector<unsigned char> &bitmap, int block_num, int block_size) {
    fs.seekg(block_num * block_size, std::ios::beg);
    fs.read(reinterpret_cast<char*>(bitmap.data()), block_size);
}

void fix_inode_bitmap(std::ifstream &fs, std::ofstream &fs_out, ext2_super_block &sb, std::vector<ext2_block_group_descriptor> &bgds, int block_size) {
    std::vector<unsigned char> inode_bitmap(block_size);
    ext2_inode inode;

    for (size_t i = 0; i < bgds.size(); ++i) {
        read_bitmap(fs, inode_bitmap, bgds[i].inode_bitmap, block_size);

        for (size_t j = 0; j < sb.inodes_per_group; ++j) {
            int inode_index = j + i * sb.inodes_per_group;
            int inode_block = bgds[i].inode_table + (j * sb.inode_size) / block_size;
            int inode_offset = (j * sb.inode_size) % block_size;

            fs.seekg(inode_block * block_size + inode_offset, std::ios::beg);
            fs.read(reinterpret_cast<char*>(&inode), sizeof(ext2_inode));

            if (inode.size > 0 && inode.link_count > 0) {
                inode_bitmap[j / 8] |= (1 << (j % 8));
            }
        }

        fs_out.seekp(bgds[i].inode_bitmap * block_size, std::ios::beg);
        fs_out.write(reinterpret_cast<char*>(inode_bitmap.data()), block_size);
    }
}

void fix_block_bitmap(std::ifstream &fs, std::ofstream &fs_out, ext2_super_block &sb, std::vector<ext2_block_group_descriptor> &bgds, int block_size) {
    std::vector<unsigned char> block_bitmap(block_size);
    ext2_inode inode;

    for (size_t i = 0; i < bgds.size(); ++i) {
        read_bitmap(fs, block_bitmap, bgds[i].block_bitmap, block_size);

        for (size_t j = 0; j < sb.inodes_per_group; ++j) {
            int inode_index = j + i * sb.inodes_per_group;
            int inode_block = bgds[i].inode_table + (j * sb.inode_size) / block_size;
            int inode_offset = (j * sb.inode_size) % block_size;

            fs.seekg(inode_block * block_size + inode_offset, std::ios::beg);
            fs.read(reinterpret_cast<char*>(&inode), sizeof(ext2_inode));

            if (inode.size > 0 && inode.link_count > 0) {
                for (int k = 0; k < 12; ++k) {
                    if (inode.direct_blocks[k] != 0) {
                        block_bitmap[inode.direct_blocks[k] / 8] |= (1 << (inode.direct_blocks[k] % 8));
                    }
                }
                // Handle indirect blocks similarly...
            }
        }

        fs_out.seekp(bgds[i].block_bitmap * block_size, std::ios::beg);
        fs_out.write(reinterpret_cast<char*>(block_bitmap.data()), block_size);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <image_file>\n";
        return 1;
    }

    std::ifstream fs(argv[1], std::ios::in | std::ios::binary);
    if (!fs) {
        std::perror("Failed to open filesystem image");
        return 1;
    }

    std::ofstream fs_out(argv[1], std::ios::in | std::ios::out | std::ios::binary);
    if (!fs_out) {
        std::perror("Failed to open filesystem image for writing");
        return 1;
    }

    ext2_super_block sb;
    read_superblock(fs, sb);

    int block_size = 1024 << sb.log_block_size;
    int num_groups = (sb.block_count + sb.blocks_per_group - 1) / sb.blocks_per_group;
    std::vector<ext2_block_group_descriptor> bgds(num_groups);

    for (int i = 0; i < num_groups; ++i) {
        read_block_group_descriptor(fs, i, bgds[i], block_size);
    }

    fix_inode_bitmap(fs, fs_out, sb, bgds, block_size);
    fix_block_bitmap(fs, fs_out, sb, bgds, block_size);

    fs.close();
    fs_out.close();

    return 0;
}
