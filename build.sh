#clang++ -ferror-limit=2 \
#	-g -O0 -march=native -Wall -Wextra -Wswitch-enum -std=c++1y \
#	main.cpp bufferedreader.cpp
#g++ -g -O0 -march=native -std=c++17 main.cpp

#g++ -O3 -march=native -std=c++17 main.cpp
g++ -DNDEBUG -O3 -march=native -std=c++17 main.cpp
