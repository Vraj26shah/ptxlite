// ptxlite-objdump - disassembles a .pobj file (the "cuobjdump" of this project).
#include "ObjectFile.h"

#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: ptxlite-objdump <file.pobj>\n";
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::cerr << "ptxlite-objdump: cannot open '" << argv[1] << "'\n";
        return 1;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    ptxl::ObjModule obj;
    std::string error;
    if (!ptxl::deserializeObject(bytes, obj, error)) {
        std::cerr << "ptxlite-objdump: " << argv[1] << ": " << error << '\n';
        return 1;
    }
    std::cout << ptxl::disassemble(obj);
    return 0;
}
