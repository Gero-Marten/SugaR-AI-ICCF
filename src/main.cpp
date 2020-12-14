/*
  SugaR, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2020 The Stockfish developers (see AUTHORS file)

  SugaR is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  SugaR is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <iostream>

#include "bitboard.h"
#include "endgame.h"
#include "misc.h"
#include "polybook.h"
#include "position.h"
#include "search.h"
#include "thread.h"
#include "tt.h"
#include "uci.h"
#include "experience.h"
#include "syzygy/tbprobe.h"

namespace PSQT {
  void init();
}

int main(int argc, char* argv[]) {

  Utility::init(argv[0]);
  show_logo();

  std::cout << engine_info() << std::endl;

  CommandLine::init(argc, argv);

  std::cout
      << "Operating System (OS) : " << os_info() << std::endl
      << "NUMA Nodes            : " << numa_nodes() << std::endl
      << "CPU Brand             : " << processor_brand() << std::endl
      << "Memory installed (RAM): " << total_memory() << std::endl
      << "Cores                 : " << physical_cores() << std::endl
      << "Threads               : " << logical_cores() << std::endl
      << "Hyper-Threading       : " << is_hyper_threading() << std::endl
      << "L1/L2/L3 cache size   : " << cache_size(0) << "/" << cache_size(1) << "/" << cache_size(2) << std::endl;

  UCI::init(Options);
  Tune::init();
  PSQT::init();
  Bitboards::init();
  Position::init();
  Bitbases::init();
  Endgames::init();
  Experience::init();
  Threads.set(size_t(Options["Threads"]));
  Threads.setFull(Options["BruteForceSearch"]);
  polybook.init(Options["BookFile"]);
  polybook2.init(Options["BookFile2"]);
  Search::clear(); // After threads are up
  Eval::NNUE::init();

  UCI::loop(argc, argv);

  Experience::unload();
  Threads.set(0);
  return 0;
}
