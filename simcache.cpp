/*
CS-UY 2214
Adapted from Jeff Epstein
Starter code for E20 cache Simulator
simcache.cpp
*/

#include <cstddef>
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <limits>
#include <iomanip>
#include <regex>
#include <cstdlib>
#include <cstdint>
#include <math.h>

using namespace std;

//Some helpful constant values that we'll be using.
size_t const static NUM_REGS = 8;
size_t const static MEM_SIZE = 1<<13;
size_t const static REG_SIZE = 1<<16;

/*
    Loads an E20 machine code file into the list
    provided by mem. We assume that mem is
    large enough to hold the values in the machine
    code file.

    @param f Open file to read from
    @param mem Array represetnting memory into which to read program
*/
void load_machine_code(ifstream &f, uint16_t mem[]) {
    regex machine_code_re("^ram\\[(\\d+)\\] = 16'b(\\d+);.*$");
    size_t expectedaddr = 0;
    string line;
    while (getline(f, line)) {
        smatch sm;
        if (!regex_match(line, sm, machine_code_re)) {
            cerr << "Can't parse line: " << line << endl;
            exit(1);
        }
        size_t addr = stoi(sm[1], nullptr, 10);
        uint16_t instr = stoi(sm[2], nullptr, 2);
        if (addr != expectedaddr) {
            cerr << "Memory addresses encountered out of sequence: " << addr << endl;
            exit(1);
        }
        if (addr >= MEM_SIZE) {
            cerr << "Program too big for memory" << endl;
            exit(1);
        }
        expectedaddr ++;
        mem[addr] = instr;
    }
}

/*
    Prints out the correctly-formatted configuration of a cache.

    @param cache_name The name of the cache. "L1" or "L2"

    @param size The total size of the cache, measured in memory cells.
        Excludes metadata

    @param assoc The associativity of the cache. One of [1,2,4,8,16]

    @param blocksize The blocksize of the cache. One of [1,2,4,8,16,32,64])

    @param num_rows The number of rows in the given cache.
*/
void print_cache_config(const string &cache_name, int size, int assoc, int blocksize, int num_rows) {
    cout << "Cache " << cache_name << " has size " << size <<
        ", associativity " << assoc << ", blocksize " << blocksize <<
        ", rows " << num_rows << endl;
}

/*
    Prints out a correctly-formatted log entry.

    @param cache_name The name of the cache where the event
        occurred. "L1" or "L2"

    @param status The kind of cache event. "SW", "HIT", or
        "MISS"

    @param pc The program counter of the memory
        access instruction

    @param addr The memory address being accessed.

    @param row The cache row or set number where the data
        is stored.
*/
void print_log_entry(const string &cache_name, const string &status, int pc, int addr, int row) {
    cout << left << setw(8) << cache_name + " " + status <<  right <<
        " pc:" << setw(5) << pc <<
        "\taddr:" << setw(5) << addr <<
        "\trow:" << setw(4) << row << endl;
}

//A struct that represents a block. It stores a tag and the clock cycle during which it was created
//The clock cycle stamp is used to check later if the block is the least recently used one, for eviction purposes
struct block
{
    uint32_t clock_cycle_stamp = 0;
    uint16_t tag = -1;
};

//A struct that represents a row. It contains a vector of blocks
struct row
{
    vector<block> My_blocks;
};

//A struct that represents a cache level. It stores the size of the cache, the associativity, and block size.
//It also stores a vector of rows.
struct level
{
    int cache_size = 0;
    int associativity = 0;
    int block_size = 0;
    vector<row> My_rows;
};

//A struct that contains a vector of cache levels.
struct cache
{
    vector<level> My_levels;
};

/**
    Main function
    Takes command-line args as documented below
*/
int main(int argc, char *argv[])
{
    /*
        Parse the command-line arguments
    */
    char *filename = nullptr;
    bool do_help = false;
    bool arg_error = false;
    string cache_config;
    for (int i=1; i<argc; i++) {
        string arg(argv[i]);
        if (arg.rfind("-",0)==0) {
            if (arg== "-h" || arg == "--help")
                do_help = true;
            else if (arg=="--cache") {
                i++;
                if (i>=argc)
                    arg_error = true;
                else
                    cache_config = argv[i];
            }
            else
                arg_error = true;
        } else {
            if (filename == nullptr)
                filename = argv[i];
            else
                arg_error = true;
        }
    }
    /* Display error message if appropriate */
    if (arg_error || do_help || filename == nullptr) {
        cerr << "usage " << argv[0] << " [-h] [--cache CACHE] filename" << endl << endl;
        cerr << "Simulate E20 cache" << endl << endl;
        cerr << "positional arguments:" << endl;
        cerr << "  filename    The file containing machine code, typically with .bin suffix" << endl<<endl;
        cerr << "optional arguments:"<<endl;
        cerr << "  -h, --help  show this help message and exit"<<endl;
        cerr << "  --cache CACHE  Cache configuration: size,associativity,blocksize (for one"<<endl;
        cerr << "                 cache) or"<<endl;
        cerr << "                 size,associativity,blocksize,size,associativity,blocksize"<<endl;
        cerr << "                 (for two caches)"<<endl;
        return 1;
    }

    //Opens file and checks if file can be opened
    ifstream f(filename);
    if (!f.is_open()) {
        cerr << "Can't open file "<<filename<<endl;
        return 1;
    }

    //Initializes my processor state, including the program counter, the general-purpose registers, and memory
    //Everything is uint16_t to let overflow wrap around
    uint16_t memory[MEM_SIZE] = {};
    uint16_t pc = 0;
    uint16_t regs[REG_SIZE] = {};

    //Loads f and parses using load_machine_code
    load_machine_code(f,memory);

    //A variable that keeps track of the clock cycle. Is useful for knowing which block is the least recently used.
    uint16_t clock_cycle = 0; 

    //A struct which represents my cache. This contains levels of cache, rows, blocks, etc.
    cache My_cache;

    /* parse cache config */
    if (cache_config.size() > 0)
    {
        vector<int> parts;
        size_t pos;
        size_t lastpos = 0;
        while ((pos = cache_config.find(",", lastpos)) != string::npos)
        {
            parts.push_back(stoi(cache_config.substr(lastpos,pos)));
            lastpos = pos + 1;
        }
        parts.push_back(stoi(cache_config.substr(lastpos)));
        if (parts.size() == 3)
        {
            int L1size = parts[0];
            int L1assoc = parts[1];
            int L1blocksize = parts[2];

            //A variable that stores the number of rows for L1
            uint16_t num_of_rows = L1size / (L1blocksize * L1assoc);

            //Initializes my cache with one level
            My_cache.My_levels.emplace_back();

            //Assigns the associativity, block size, and cache size to my L1 cache
            My_cache.My_levels[0].associativity = L1assoc;
            My_cache.My_levels[0].block_size = L1blocksize;
            My_cache.My_levels[0].cache_size = L1size;

            //A for loop that creates empty rows in my L1 cache equal to the number of rows, which was calculated earlier
            for(uint16_t i = 0; i < num_of_rows; i++)
            {
                My_cache.My_levels[0].My_rows.emplace_back();
            }
        }
        else if (parts.size() == 6)
        {
            int L1size = parts[0];
            int L1assoc = parts[1];
            int L1blocksize = parts[2];
            int L2size = parts[3];
            int L2assoc = parts[4];
            int L2blocksize = parts[5];

            //A variable that stores the number of rows for L1
            uint16_t num_of_rows = L1size / (L1blocksize * L1assoc);

            //Initializes my cache with one level
            My_cache.My_levels.emplace_back();
            
            //Assigns the associativity, block size, and cache size to my L1 cache
            My_cache.My_levels[0].associativity = L1assoc;
            My_cache.My_levels[0].block_size = L1blocksize;
            My_cache.My_levels[0].cache_size = L1size;

            //A for loop that creates empty rows in my L1 cache equal to the number of rows, which was calculated earlier
            for(uint16_t i = 0; i < num_of_rows; i++)
            {
                My_cache.My_levels[0].My_rows.emplace_back();
            }

            //A variable that stores the number of rows for L2
            num_of_rows = L2size / (L2blocksize * L2assoc);

            //Creates an L2 cache in my levels vector
            My_cache.My_levels.emplace_back();

            //Assigns associativity, blocksize, and size to my L2 cache
            My_cache.My_levels[1].associativity = L2assoc;
            My_cache.My_levels[1].block_size = L2blocksize;
            My_cache.My_levels[1].cache_size = L2size;

            //A for loop that creates empty rows in my L2 cache equal to the number of rows, which was calculated earlier
            for(uint16_t j = 0; j < num_of_rows; j++)
            {
                My_cache.My_levels[1].My_rows.emplace_back();
            }
        }
        else
        {
            cerr << "Invalid cache config"  << endl;
            return 1;
        }

        //Prints out the cache configuration for the levels in the cache
        for(int i = 0; i < My_cache.My_levels.size(); i++)
        {
            print_cache_config("L" + to_string(i+1), My_cache.My_levels[i].cache_size, My_cache.My_levels[i].associativity, My_cache.My_levels[i].block_size, My_cache.My_levels[i].My_rows.size());
        }

        //Checks if a halt command exists
        bool halt_checker = false;

        //Stores the opcode
        uint16_t opcode = 0;

        //Stores the last four bits
        uint16_t LSB_4 = 0;

        //Program stops checking for instructions once a halt is detected
        while(!halt_checker)
        {
            //Gets opcode and last four bits in 16_bit binary
            opcode = memory[pc % MEM_SIZE] >> 13; 
            LSB_4 = (memory[pc % MEM_SIZE] & 0b1111);

            //Instructions With Three Register Arguments
            //All of these instructions have an opcode of 000
            //Each instructions is differentiated by their last four bits
            if(opcode == 0b000)
            {
                //I made pointer variables that represent the three registers used
                //I can use these variables to update the registers
                uint16_t* regA = &regs[((memory[pc % MEM_SIZE] & 0b0001110000000000)>>10)];
                uint16_t* regB = &regs[((memory[pc % MEM_SIZE] & 0b0000001110000000)>>7)];
                uint16_t* regDst = &regs[((memory[pc % MEM_SIZE] & 0b0000000001110000)>>4)];

                //add instruction: adds values in register A and B and stores the result in the destination register
                if(LSB_4 == 0b0000)
                {
                    *regDst = *regA + *regB;
                }
                //sub instruction: subtracts values in register A and B and stores the result in the destination register
                else if(LSB_4 == 0b0001)
                {
                    *regDst = *regA - *regB;
                }
                //or instruction: performs bitwise OR operation on values in register A and B and stores the result in the destination register
                else if(LSB_4 == 0b0010)
                {
                    *regDst = *regA | *regB;
                }
                //and instruction: performs bitwise AND operation on values in register A and B and stores the result in the destination register
                else if(LSB_4 == 0b0011)
                {
                    *regDst = *regA & *regB;
                }
                //slt instruction: checks if register A is less than register B and stores 1 in the destination register if true and 0 in the
                //destination register if false
                //The operands are unsigned 16-bit
                else if(LSB_4 == 0b0100)
                {
                    *regDst = (*regA < *regB);
                }
                //jr instruction: jumps to a memory address that is the value in register A
                //I subtract the value given by the register by 1, since I add 1 at the end
                //Checks if bits 9 to 4 are 0, in case an invalid instruction was created using store word
                //Any other 16 bit instruction created by store word in my program should have an associated instruction or be ignored
                else if((LSB_4 == 0b1000) && (memory[pc % MEM_SIZE] & 0b0000001111110000) == 0)
                {
                    pc = *regA - 1;
                }
                //increments pc counter by 1
                pc += 1;

            }
            //Instructions With No Register Arguments
            //All of these instructions have their first two bits as 01
            //Each instruction is differentiated by their opcode
            else if((opcode & 0b110)  == 0b010)
            {
                //j imm instruction: jumps unconditionally to a memory address that is the immediate value
                if(opcode == 0b010)
                {
                    //Remembers the last pc value, performs a jump, and if the jumped pc value is the same as the previous one,
                    //The halt checker becomes true. This means a halt instruction was called
                    uint16_t last_pc = pc;
                    pc = (memory[pc % MEM_SIZE] & 0b0001111111111111);
                    if(last_pc == pc)
                    {
                        halt_checker = true;
                    }
                }
                //jal instruction: stores the next memory address in register 7 and then jumps unconditionally to a memory address
                //that is the immediate value
                else if(opcode == 0b011)
                {
                    regs[7] = pc+1;
                    pc = (memory[pc % MEM_SIZE] & 0b0001111111111111);
                }
            }
            //Instructions With Two Register Arguments
            //Each instruction is differentiated by their opcode
            else
            {
                //I made variables that represents the two registers that can represent regA, regB,
                //regSrc, regDst, or regAddr, depending on the instrucion. Also a variable that
                //stores the immediate
                uint16_t* regA = &regs[((memory[pc % MEM_SIZE] & 0b0001110000000000)>>10)];
                uint16_t* regB = &regs[((memory[pc % MEM_SIZE] & 0b0000001110000000)>>7)];
                uint16_t imm = (memory[pc % MEM_SIZE] & 0b0000000001111111);

                //Sign extends my immediate so that any immediate operations use a sign extended immediate
                if((imm & 0b1000000) == 0b1000000)
                {
                    imm = imm ^ 0b1111111110000000;
                }

                //slti instruction: checks if register A is less than the immediate value and stores 1 in register B if true
                //and 0 in register B if false
                if(opcode == 0b111)
                {
                    *regB = (*regA < imm);
                }
                //lw instruction: stores the instruction from the memory address at register A plus the immediate value in register B
                else if(opcode == 0b100)
                {
                    *regB = memory[(*regA + imm) % MEM_SIZE];

                    //Variables to store blockID, row, and tag. Will be calculated later in a while loop
                    uint16_t blockID = 0;
                    uint16_t row = 0;
                    uint16_t tag = 0;

                    //A variable that tracks if the needed block is found or not
                    uint16_t block_found = false;

                    //A variable that is the max value of a uint64_t. This is so it is higher than the clock_cycle_stamps of all blocks which are uint32_t's
                    //Will be used to compare block clock_cycle_stamps with each other to find the smallest one, which is the least recently used
                    uint64_t curr_LRU = UINT64_MAX;

                    //A variable that keeps track of the index of the least recently used block
                    uint16_t LRU = 0;

                    //A variable that keeps track of the current level being used
                    uint16_t curr_level = 0;

                    //A while loop that will determine if any hits were found. Ends early if block is found
                    while(curr_level < My_cache.My_levels.size() && !block_found)
                    {
                        //BlockID, row, and tag are calculated here and used to determine hits
                        //curr_LRU and LRU are reset here for when we loop through each level
                        blockID = ((*regA + imm) % MEM_SIZE) / My_cache.My_levels[curr_level].block_size;
                        row = blockID % My_cache.My_levels[curr_level].My_rows.size();
                        tag = blockID / My_cache.My_levels[curr_level].My_rows.size();
                        curr_LRU = UINT64_MAX;
                        LRU = 0;

                        //Loops through each block in the calculated row to see if any of them have the needed tag
                        for(uint16_t i = 0; i < My_cache.My_levels[curr_level].My_rows[row].My_blocks.size(); i++)
                        {
                            //Everytime a block with an earlier clock_cycle_stamp is found, its index is recorded so it can
                            //be evicted later if there is a miss and the block size is the same as associativity
                            if(My_cache.My_levels[curr_level].My_rows[row].My_blocks[i].clock_cycle_stamp < curr_LRU)
                            {
                                curr_LRU = My_cache.My_levels[curr_level].My_rows[row].My_blocks[i].clock_cycle_stamp;
                                LRU = i;
                            }

                            //A hit or miss if statement. It checks if the block we are looking at has the needed tag
                            if(My_cache.My_levels[curr_level].My_rows[row].My_blocks[i].tag == tag)
                            {
                                //If a hit happens, this updates the clock_cycle_stamp of the hit block to be the current clock_cycle
                                //and block_found is changed to true.
                                My_cache.My_levels[curr_level].My_rows[row].My_blocks[i].clock_cycle_stamp = clock_cycle;
                                block_found = true;

                                //Prints a log entry indicating a hit at the current level, calculated row, and pc counter. Then ends the for loop early, since the block is found
                                print_log_entry("L" + to_string(curr_level + 1), "HIT", pc, ((*regA + imm) % MEM_SIZE), row);
                                i = My_cache.My_levels[curr_level].My_rows[row].My_blocks.size();
                            }
                        }

                        //If the block has not been found, this will indicate any misses and create blocks and/or evict blocks
                        if(!block_found)
                        {
                            //Prints a log entry indicating a miss at the current level, calculated row, and pc counter
                            print_log_entry("L" + to_string(curr_level + 1), "MISS", pc, ((*regA + imm) % MEM_SIZE), row);

                            //If the max amount of blocks is reached, the least recently used block is evicted
                            if(My_cache.My_levels[curr_level].My_rows[row].My_blocks.size() == My_cache.My_levels[curr_level].associativity)
                            {
                                My_cache.My_levels[curr_level].My_rows[row].My_blocks.erase(My_cache.My_levels[curr_level].My_rows[row].My_blocks.begin() + LRU);
                            }

                            //A new block is added to the row with the calculated tag and current block cycle
                            block new_block = {clock_cycle, tag};
                            My_cache.My_levels[curr_level].My_rows[row].My_blocks.push_back(new_block);

                            //The level counter is incremented to convey that we are moving to the next level of cache
                            curr_level++;
                        }
                        //If the block has been found
                        else
                        {
                            //Ends the while loop early because the block has already been found
                            curr_level = My_cache.My_levels.size();
                        }
                    }
                }
                //sw instruction: changes the instruction at the memory address at register A plus the immediate value to the value in register B
                else if(opcode == 0b101)
                {
                    memory[(*regA + imm) % MEM_SIZE] = *regB;

                    //Variables to store blockID, row, and tag. Will be calculated later in a while loop
                    uint16_t blockID = 0;
                    uint16_t row = 0;
                    uint16_t tag = 0;

                    //A variable that tracks if the needed block is found or not
                    uint16_t block_found = false;

                    //A variable that is the max value of a uint64_t. This is so it is higher than the clock_cycle_stamps of all blocks which are uint32_t's
                    //Will be used to compare block clock_cycle_stamps with each other to find the smallest one, which is the least recently used
                    uint64_t curr_LRU = UINT64_MAX;

                    //A variable that keeps track of the index of the least recently used block
                    uint16_t LRU = 0;

                    //A variable that keeps track of the current level being used
                    uint16_t curr_level = 0;

                    //A while loop that will determine if any hits were found. Does not end early if block is found, since write through will write to all cache levels
                    while(curr_level < My_cache.My_levels.size())
                    {
                        //BlockID, row, and tag are calculated here and used to determine hits
                        //curr_LRU and LRU are reset here for when we loop through each level
                        blockID = ((*regA + imm) % MEM_SIZE) / My_cache.My_levels[curr_level].block_size;
                        row = blockID % My_cache.My_levels[curr_level].My_rows.size();
                        tag = blockID / My_cache.My_levels[curr_level].My_rows.size();
                        curr_LRU = UINT64_MAX;
                        LRU = 0;

                        //Loops through each block in the calculated row to see if any of them have the needed tag
                        for(uint16_t i = 0; i < My_cache.My_levels[curr_level].My_rows[row].My_blocks.size(); i++)
                        {
                            //Everytime a block with an earlier clock_cycle_stamp is found, its index is recorded so it can
                            //be evicted later if there is a miss and the block size is the same as associativity
                            if(My_cache.My_levels[curr_level].My_rows[row].My_blocks[i].clock_cycle_stamp < curr_LRU)
                            {
                                curr_LRU = My_cache.My_levels[curr_level].My_rows[row].My_blocks[i].clock_cycle_stamp;
                                LRU = i;
                            }

                            //A hit or miss if statement. It checks if the block we are looking at has the needed tag
                            if(My_cache.My_levels[curr_level].My_rows[row].My_blocks[i].tag == tag)
                            {
                                //If a hit happens, this updates the clock_cycle_stamp of the hit block to be the current clock_cycle
                                //and block_found is changed to true.
                                My_cache.My_levels[curr_level].My_rows[row].My_blocks[i].clock_cycle_stamp = clock_cycle;
                                block_found = true;

                                //Ends the for loop early, since the block is found
                                i = My_cache.My_levels[curr_level].My_rows[row].My_blocks.size();
                            }
                        }

                        //No matter if a hit or miss occurs, a store word instruction will occur.
                        //Prints a log entry indicating a SW at the current level, calculated row, and pc counter
                        print_log_entry("L" + to_string(curr_level + 1), "SW", pc, ((*regA + imm) % MEM_SIZE), row);
                        
                        //If the block has not been found, this will indicate any misses and create blocks and/or evict blocks
                        if(!block_found)
                        {
                            //If the max amount of blocks is reached, the least recently used block is evicted
                            if(My_cache.My_levels[curr_level].My_rows[row].My_blocks.size() == My_cache.My_levels[curr_level].associativity)
                            {
                                My_cache.My_levels[curr_level].My_rows[row].My_blocks.erase(My_cache.My_levels[curr_level].My_rows[row].My_blocks.begin() + LRU);
                            }

                            //A new block is added to the row with the calculated tag and current block cycle
                            block new_block = {clock_cycle, tag};
                            My_cache.My_levels[curr_level].My_rows[row].My_blocks.push_back(new_block);
                        }
                                                    
                        //The level counter is incremented to convey that we are moving to the next level of cache
                        curr_level++;
                    }
                }
                //jeq instruction: checks if the value in registers A and B are equal. If so, jump to the immediate value + pc.
                //It does not add one, since one is added at the end
                else if(opcode == 0b110)
                {
                    if(*regA == *regB)
                    {
                        pc = imm + pc;
                    }
                }
                //addi instruction: adds the value in register A to the immediate value and stores the result in register B
                else if(opcode == 0b001)
                {
                    *regB = *regA + imm;
                }
                //increments pc counter by 1
                pc += 1;
            }

            //increments clock cycle
            clock_cycle++;

            //resets register 0 to 0
            regs[0] = 0;
        }

    }

    return 0;
}
//ra0Eequ6ucie6Jei0koh6phishohm9
