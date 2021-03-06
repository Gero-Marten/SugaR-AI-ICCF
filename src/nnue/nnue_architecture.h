/*
  SugaR, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2021 The Stockfish developers (see AUTHORS file)

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

// Input features and network structure used in NNUE evaluation function

#ifndef NNUE_ARCHITECTURE_H_INCLUDED
#define NNUE_ARCHITECTURE_H_INCLUDED

// Defines the network structure
#include "architectures/halfkp_256x2-32-32.h"

namespace Stockfish::Eval::NNUE {

  static_assert(kTransformedFeatureDimensions % kMaxSimdWidth == 0, "");
  static_assert(Network::kOutputDimensions == 1, "");
  static_assert(std::is_same<Network::OutputType, std::int32_t>::value, "");

  // Trigger for full calculation instead of difference calculation
  constexpr auto kRefreshTriggers = RawFeatures::kRefreshTriggers;

}  // namespace Stockfish::Eval::NNUE

#endif // #ifndef NNUE_ARCHITECTURE_H_INCLUDED
