/*
  This file is part of Leela Chess Zero.
  Copyright (C) 2018 The LCZero Authors

  Leela Chess is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Leela Chess is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Leela Chess.  If not, see <http://www.gnu.org/licenses/>.

  Additional permission under GNU GPL version 3 section 7

  If you modify this Program, or any covered work, by linking or
  combining it with NVIDIA Corporation's libraries from the NVIDIA CUDA
  Toolkit and the NVIDIA CUDA Deep Neural Network library (or a
  modified version of those libraries), containing parts covered by the
  terms of the respective license agreement, the licensors of this
  Program grant you additional permission to convey the resulting work.
*/
#include <sstream>
#include <iostream>
#include <iomanip>

#include "selfplay/loop.h"
#include "selfplay/tournament.h"
#include "utils/configfile.h"

namespace lczero {

namespace {
const OptionId kInteractiveId{
    "interactive", "", "Run in interactive mode with UCI-like interface."};
}  // namespace

SelfPlayLoop::SelfPlayLoop() {}

SelfPlayLoop::~SelfPlayLoop() {
  if (tournament_) tournament_->Abort();
  if (thread_) thread_->join();
}

void SelfPlayLoop::RunLoop() {
  options_.Add<BoolOption>(kInteractiveId) = false;
  SelfPlayTournament::PopulateOptions(&options_);

  if (!options_.ProcessAllFlags()) return;
  if (options_.GetOptionsDict().Get<bool>(kInteractiveId.GetId())) {
    UciLoop::RunLoop();
  } else {
    // Send id before starting tournament to allow wrapping client to know
    // who we are.
    SendId();
    SelfPlayTournament tournament(
        options_.GetOptionsDict(),
        std::bind(&UciLoop::SendBestMove, this, std::placeholders::_1),
        std::bind(&UciLoop::SendInfo, this, std::placeholders::_1),
        std::bind(&SelfPlayLoop::SendGameInfo, this, std::placeholders::_1),
        std::bind(&SelfPlayLoop::SendTournament, this, std::placeholders::_1));
    tournament.RunBlocking();
  }
}

void SelfPlayLoop::CmdUci() {
  SendId();
  for (const auto& option : options_.ListOptionsUci()) {
    SendResponse(option);
  }
  SendResponse("uciok");
}

void SelfPlayLoop::CmdStart() {
  if (tournament_) return;
  tournament_ = std::make_unique<SelfPlayTournament>(
      options_.GetOptionsDict(),
      std::bind(&UciLoop::SendBestMove, this, std::placeholders::_1),
      std::bind(&UciLoop::SendInfo, this, std::placeholders::_1),
      std::bind(&SelfPlayLoop::SendGameInfo, this, std::placeholders::_1),
      std::bind(&SelfPlayLoop::SendTournament, this, std::placeholders::_1));
  thread_ =
      std::make_unique<std::thread>([this]() { tournament_->RunBlocking(); });
}

void SelfPlayLoop::SendGameInfo(const GameInfo& info) {
  std::vector<std::string> responses;
  // Send separate resign report before gameready as client gameready parsing
  // will easily get confused by adding new parameters as both training file
  // and move list potentially contain spaces.
  if (info.min_false_positive_threshold) {
    std::string resign_res = "resign_report";
    resign_res +=
        " fp_threshold " + std::to_string(*info.min_false_positive_threshold);
    responses.push_back(resign_res);
  }
  std::string res = "gameready";
  if (!info.training_filename.empty())
    res += " trainingfile " + info.training_filename;
  if (info.game_id != -1) res += " gameid " + std::to_string(info.game_id);
  if (info.is_black)
    res += " player1 " + std::string(*info.is_black ? "black" : "white");
  if (info.game_result != GameResult::UNDECIDED) {
    res += std::string(" result ") +
           ((info.game_result == GameResult::DRAW)
                ? "draw"
                : (info.game_result == GameResult::WHITE_WON) ? "whitewon"
                                                              : "blackwon");
  }
  if (!info.moves.empty()) {
    res += " moves";
    for (const auto& move : info.moves) res += " " + move.as_string();
  }
  responses.push_back(res);
  SendResponses(responses);
}

void SelfPlayLoop::CmdSetOption(const std::string& name,
                                const std::string& value,
                                const std::string& context) {
  options_.SetUciOption(name, value, context);
}

void SelfPlayLoop::SendTournament(const TournamentInfo& info) {
  const int winp1 = info.results[0][0] + info.results[0][1];
  const int loosep1 = info.results[2][0] + info.results[2][1];
  const int draws = info.results[1][0] + info.results[1][1];

  // Initialize variables
  float percentage = -1;
  float elo = 10000;
  float los = 10000;

  // Only caculate percentage if any games at all (avoid divide by 0).
  if ((winp1 + loosep1 + draws) > 0)
    percentage =
        (static_cast<float>(draws) / 2 + winp1) / (winp1 + loosep1 + draws);

  // Calculate elo and los if percentage strictly between 0 and 1 (avoids divide
  // by 0 or overflow).
  if ((percentage < 1) && (percentage > 0))
    elo = -400 * log(1 / percentage - 1) / log(10);
  if ((winp1 + loosep1) > 0)
    los = .5 +
          .5 * std::erf((winp1 - loosep1) / std::sqrt(2.0 * (winp1 + loosep1)));

  std::string res = "tournamentstatus";
  if (info.finished) res += " final";
  res += " P1: +" + std::to_string(winp1) + " -" + std::to_string(loosep1) +
         " =" + std::to_string(draws);

  if (percentage > 0) {
    std::ostringstream oss;
    oss << std::fixed << std::setw(5) << std::setprecision(2)
        << (percentage * 100) << "%";
    res += " Win: " + oss.str();
  }
  if (elo < 10000) {
    std::ostringstream oss;
    oss << std::fixed << std::setw(5) << std::setprecision(2) << (elo);
    res += " Elo: " + oss.str();
  }
  if (los < 10000) {
    std::ostringstream oss;
    oss << std::fixed << std::setw(5) << std::setprecision(2) << (los * 100)
        << "%";
    res += " LOS: " + oss.str();
  }
  res += " P1-W: +" + std::to_string(info.results[0][0]) + " -" +
         std::to_string(info.results[2][0]) + " =" +
         std::to_string(info.results[1][0]);
  // Might be redundant to also list P1-B:
  res += " P1-B: +" + std::to_string(info.results[0][1]) + " -" +
         std::to_string(info.results[2][1]) + " =" +
         std::to_string(info.results[1][1]);
  SendResponse(res);
}

}  // namespace lczero
