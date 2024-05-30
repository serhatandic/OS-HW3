#include <iostream>
#include <fstream>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include "ext2fs.h"
#include "identifier.h"
#include "ext2fs_print.h"
#include <algorithm>

#define EXT2_BLOCK_SIZE(sb) (1024 << (sb).log_block_size)

class Ext2Recovery {
public:
    Ext2Recovery(const std::string &imagePath, const std::vector<uint8_t> &dataIdentifier)
        : imagePath(imagePath), dataIdentifier(dataIdentifier) {
        fd = open(imagePath.c_str(), O_RDWR);
        if (fd == -1) {
            throw std::runtime_error("Failed to open image file");
        }
        fetchSuperblock();
    }

    ~Ext2Recovery() {
        close(fd);
    }

    void recover() {
        printSuperBlock();
        recoverInodeBitmaps();
        recoverBlockBitmaps();
    }

private:
    int fd;
    std::string imagePath;
    std::vector<uint8_t> dataIdentifier;
    struct ext2_super_block superBlock;

    void fetchSuperblock() {
        lseek(fd, 1024, SEEK_SET);
        read(fd, &superBlock, sizeof(struct ext2_super_block));
    }

    void printSuperBlock() {
        print_super_block(&superBlock);
    }

    void recoverInodeBitmaps() {
        int totalInodeBitmapSize = (superBlock.inode_count + 7) / 8;
        std::vector<char> aggregatedInodeBitmap(totalInodeBitmapSize, 0);

        for (int i = 0; i < 11; ++i) {
            aggregatedInodeBitmap[i / 8] |= (1 << (i % 8));
        }

        struct ext2_inode inode;
        for (int i = 1; i <= superBlock.inode_count; ++i) {
            readInode(i, &inode);
            if (inode.link_count > 0) {
                aggregatedInodeBitmap[(i - 1) / 8] |= (1 << ((i - 1) % 8));
            }
        }

        int blockGroupCount = (superBlock.block_count + superBlock.blocks_per_group - 1) / superBlock.blocks_per_group;
        std::vector<char> inodeBitmap((superBlock.inodes_per_group + 7) / 8);

        for (int group = 0; group < blockGroupCount; ++group) {
            struct ext2_block_group_descriptor bgd;
            pread(fd, &bgd, sizeof(bgd), 1024 + EXT2_SUPER_BLOCK_SIZE + group * sizeof(bgd));

            pread(fd, inodeBitmap.data(), inodeBitmap.size(), bgd.inode_bitmap * EXT2_BLOCK_SIZE(superBlock));

            correctInodeBitmap(group, inodeBitmap, aggregatedInodeBitmap);

            pwrite(fd, inodeBitmap.data(), inodeBitmap.size(), bgd.inode_bitmap * EXT2_BLOCK_SIZE(superBlock));
        }
    }

    void recoverBlockBitmaps() {
        int blockGroupCount = (superBlock.block_count + superBlock.blocks_per_group - 1) / superBlock.blocks_per_group;
        int totalBitmapSize = (superBlock.block_count + 7) / 8;
        std::vector<char> aggregatedBitmap(totalBitmapSize, 0);
        int blockSize = EXT2_BLOCK_SIZE(superBlock);

        for (int i = 1; i <= superBlock.inode_count; ++i) {
            struct ext2_inode inode;
            readInode(i, &inode);
            updateAggregatedBitmap(inode, aggregatedBitmap);
        }

        for (int block = 0; block < superBlock.block_count; ++block) {
            std::vector<char> buffer(blockSize);
            pread(fd, buffer.data(), blockSize, block * blockSize);

            if (!isBlockEmpty(buffer)) {
                setBitInAggregatedBitmap(block, aggregatedBitmap);
            }
        }

        markMetadataBlocksUsed(aggregatedBitmap);

        for (int group = 0; group < blockGroupCount; ++group) {
            struct ext2_block_group_descriptor bgd;
            pread(fd, &bgd, sizeof(bgd), 1024 + EXT2_SUPER_BLOCK_SIZE + group * sizeof(bgd));

            std::vector<char> blockBitmap(superBlock.blocks_per_group / 8);
            pread(fd, blockBitmap.data(), blockBitmap.size(), bgd.block_bitmap * EXT2_BLOCK_SIZE(superBlock));

            correctBlockBitmap(group, blockBitmap, aggregatedBitmap);
            pwrite(fd, blockBitmap.data(), blockBitmap.size(), bgd.block_bitmap * EXT2_BLOCK_SIZE(superBlock));
        }
    }

    void correctInodeBitmap(int group, std::vector<char> &inodeBitmap, const std::vector<char> &aggregatedInodeBitmap) {
        int startInode = group * superBlock.inodes_per_group;
        int endInode = std::min(startInode + superBlock.inodes_per_group, superBlock.inode_count);

        for (int i = startInode; i < endInode; ++i) {
            int localIndex = i - startInode;
            int byteIndex = localIndex / 8;
            int bitIndex = localIndex % 8;

            bool aggregateBit = (aggregatedInodeBitmap[i / 8] >> (i % 8)) & 1;
            bool currentBit = (inodeBitmap[byteIndex] >> bitIndex) & 1;

            if (aggregateBit != currentBit) {
                inodeBitmap[byteIndex] ^= (1 << bitIndex);
            }
        }
    }

    void correctBlockBitmap(int group, std::vector<char> &blockBitmap, const std::vector<char> &aggregatedBitmap) {
        int startBlock = group * superBlock.blocks_per_group;
        int endBlock = std::min(startBlock + superBlock.blocks_per_group, superBlock.block_count);

        for (int i = startBlock; i < endBlock; ++i) {
            int byteIndex = (i - startBlock) / 8;
            int bitIndex = (i - startBlock) % 8;

            if ((aggregatedBitmap[i / 8] >> (i % 8)) & 1) {
                blockBitmap[byteIndex] |= (1 << bitIndex);
            }
        }
    }

    void markMetadataBlocksUsed(std::vector<char> &aggregatedBitmap) {
        int blockGroupCount = (superBlock.block_count + superBlock.blocks_per_group - 1) / superBlock.blocks_per_group;
        int blockSize = EXT2_BLOCK_SIZE(superBlock);

        for (int group = 0; group < blockGroupCount; ++group) {
            struct ext2_block_group_descriptor bgd;
            pread(fd, &bgd, sizeof(bgd), 1024 + EXT2_SUPER_BLOCK_SIZE + group * sizeof(bgd));

            int inodeTableSize = (superBlock.inodes_per_group + blockSize / EXT2_INODE_SIZE - 1) / (blockSize / EXT2_INODE_SIZE);
            int endBlock = bgd.inode_table + inodeTableSize;
            for (int i = group * superBlock.blocks_per_group; i < endBlock; ++i) {
                setBitInAggregatedBitmap(i, aggregatedBitmap);
            }
        }
    }

    void updateAggregatedBitmap(const struct ext2_inode &inode, std::vector<char> &aggregatedBitmap) {
        if (inode.mode == 0 || inode.link_count == 0) {
            return;
        }

        for (const auto &block : inode.direct_blocks) {
            if (block != 0) {
                setBitInAggregatedBitmap(block, aggregatedBitmap);
            }
        }

        const std::array<std::pair<uint32_t, int>, 3> indirectBlocks = {
            std::make_pair(inode.single_indirect, 1),
            std::make_pair(inode.double_indirect, 2),
            std::make_pair(inode.triple_indirect, 3)
        };

        for (const auto &[block, level] : indirectBlocks) {
            updateBitmapForIndirectBlocks(block, level, aggregatedBitmap);
            setBitInAggregatedBitmap(block, aggregatedBitmap);
        }
    }

    void updateBitmapForIndirectBlocks(uint32_t blockIndex, int level, std::vector<char> &aggregatedBitmap) {
        if (blockIndex == 0) {
            return;
        }

        int blockSize = EXT2_BLOCK_SIZE(superBlock);
        std::vector<uint32_t> blockPointers(blockSize / sizeof(uint32_t));
        pread(fd, blockPointers.data(), blockSize, blockIndex * blockSize);

        for (const auto &pointer : blockPointers) {
            if (pointer != 0) {
                setBitInAggregatedBitmap(pointer, aggregatedBitmap);
                if (level > 1) {
                    updateBitmapForIndirectBlocks(pointer, level - 1, aggregatedBitmap);
                }
            }
        }
    }

    void readInode(int inodeIndex, struct ext2_inode *inode) {
        off_t inodeTableStart = calculateInodeTableStart((inodeIndex - 1) / superBlock.inodes_per_group);
        off_t inodeOffset = ((inodeIndex - 1) % superBlock.inodes_per_group) * EXT2_INODE_SIZE;
        lseek(fd, inodeTableStart + inodeOffset, SEEK_SET);
        read(fd, inode, sizeof(struct ext2_inode));
    }

    off_t calculateInodeTableStart(int blockGroup) {
        struct ext2_block_group_descriptor bgd;
        pread(fd, &bgd, sizeof(bgd), 1024 + EXT2_SUPER_BLOCK_SIZE + blockGroup * sizeof(bgd));
        return static_cast<off_t>(bgd.inode_table * EXT2_BLOCK_SIZE(superBlock));
    }

    bool isBlockEmpty(const std::vector<char> &block) {
        return std::all_of(block.begin(), block.end(), [](char c) { return c == 0; });
    }

    void setBitInAggregatedBitmap(uint32_t blockIndex, std::vector<char> &aggregatedBitmap) {
        aggregatedBitmap[blockIndex / 8] |= (1 << (blockIndex % 8));
    }

};

int main(int argc, char *argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <image_location> <data_identifier>" << std::endl;
        return EXIT_FAILURE;
    }
    std::string imagePath = argv[1];
    uint8_t* rawIdentifier = parse_identifier(argc, argv);
    std::vector<uint8_t> dataIdentifier(rawIdentifier, rawIdentifier + (argc - 2));
    delete[] rawIdentifier;

    try {
        Ext2Recovery recovery(imagePath, dataIdentifier);
        recovery.recover();
    } catch (const std::exception &ex) {
        std::cerr << "Error: " << ex.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
