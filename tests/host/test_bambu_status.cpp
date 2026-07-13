#include "doctest/doctest.h"

#include "printsphere/bambu_status.hpp"

using namespace printsphere;

TEST_CASE("normalize_bambu_status_token strips non-alnum and uppercases") {
  CHECK(normalize_bambu_status_token("running") == "RUNNING");
  CHECK(normalize_bambu_status_token("X1-Carbon") == "X1CARBON");
  CHECK(normalize_bambu_status_token("  in_progress  ") == "INPROGRESS");
  CHECK(normalize_bambu_status_token("") == "");
}

TEST_CASE("bambu_model_from_product_name maps known marketing names") {
  CHECK(bambu_model_from_product_name("A1 mini") == PrinterModel::kA1Mini);
  CHECK(bambu_model_from_product_name("Bambu Lab A1") == PrinterModel::kA1);
  CHECK(bambu_model_from_product_name("A1") == PrinterModel::kA1);
  CHECK(bambu_model_from_product_name("P1S") == PrinterModel::kP1S);
  CHECK(bambu_model_from_product_name("P1P") == PrinterModel::kP1P);
  CHECK(bambu_model_from_product_name("P2S") == PrinterModel::kP2S);
  CHECK(bambu_model_from_product_name("H2D Pro") == PrinterModel::kH2DPro);
  CHECK(bambu_model_from_product_name("H2D") == PrinterModel::kH2D);
  CHECK(bambu_model_from_product_name("H2S") == PrinterModel::kH2S);
  CHECK(bambu_model_from_product_name("H2C") == PrinterModel::kH2C);
  CHECK(bambu_model_from_product_name("X1E") == PrinterModel::kX1E);
  CHECK(bambu_model_from_product_name("X1 Carbon") == PrinterModel::kX1C);
  CHECK(bambu_model_from_product_name("X1C") == PrinterModel::kX1C);
  CHECK(bambu_model_from_product_name("X1") == PrinterModel::kX1);
  CHECK(bambu_model_from_product_name("Toaster") == PrinterModel::kUnknown);
  CHECK(bambu_model_from_product_name("") == PrinterModel::kUnknown);
}

TEST_CASE("bambu status family predicates") {
  SUBCASE("failed family") {
    CHECK(bambu_status_is_failed("FAILED"));
    CHECK(bambu_status_is_failed("Cancelled"));
    CHECK(bambu_status_is_failed("ERROR"));
    CHECK_FALSE(bambu_status_is_failed("RUNNING"));
  }
  SUBCASE("finished family") {
    CHECK(bambu_status_is_finished("FINISH"));
    CHECK(bambu_status_is_finished("Done"));
    CHECK(bambu_status_is_finished("SUCCESS"));
    CHECK(bambu_status_is_finished("Completed"));
    CHECK_FALSE(bambu_status_is_finished("RUNNING"));
  }
  SUBCASE("paused family") {
    CHECK(bambu_status_is_paused("PAUSE"));
    CHECK(bambu_status_is_paused("paused"));
    CHECK_FALSE(bambu_status_is_paused("RUNNING"));
  }
  SUBCASE("preparing family") {
    CHECK(bambu_status_is_preparing("INIT"));
    CHECK(bambu_status_is_preparing("Slicing"));
    CHECK(bambu_status_is_preparing("Heating"));
    CHECK(bambu_status_is_preparing("Downloading"));
    CHECK(bambu_status_is_preparing("Preparing"));
    CHECK(bambu_status_is_preparing("Starting"));
    CHECK_FALSE(bambu_status_is_preparing("RUNNING"));
  }
  SUBCASE("printing family") {
    CHECK(bambu_status_is_printing("RUNNING"));
    CHECK(bambu_status_is_printing("Printing"));
    CHECK(bambu_status_is_printing("PROCESSING"));
    CHECK_FALSE(bambu_status_is_printing("PAUSE"));
  }
}

TEST_CASE("lifecycle_from_bambu_status") {
  CHECK(lifecycle_from_bambu_status("RUNNING", false) == PrintLifecycleState::kPrinting);
  CHECK(lifecycle_from_bambu_status("PAUSE", false) == PrintLifecycleState::kPaused);
  CHECK(lifecycle_from_bambu_status("FINISH", false) == PrintLifecycleState::kFinished);
  CHECK(lifecycle_from_bambu_status("FAILED", false) == PrintLifecycleState::kError);
  CHECK(lifecycle_from_bambu_status("PREPARE", false) == PrintLifecycleState::kPreparing);
  CHECK(lifecycle_from_bambu_status("IDLE", false) == PrintLifecycleState::kIdle);
  CHECK(lifecycle_from_bambu_status("OFFLINE", false) == PrintLifecycleState::kIdle);
  CHECK(lifecycle_from_bambu_status("UNKNOWN", false) == PrintLifecycleState::kUnknown);
  CHECK(lifecycle_from_bambu_status("", false) == PrintLifecycleState::kUnknown);

  // has_concrete_error wins over text
  CHECK(lifecycle_from_bambu_status("RUNNING", true) == PrintLifecycleState::kError);
  CHECK(lifecycle_from_bambu_status("FINISH", true) == PrintLifecycleState::kError);
}

TEST_CASE("bambu_pretty_status") {
  CHECK(bambu_pretty_status("Downloading model").compare("downloading") == 0);
  CHECK(bambu_pretty_status("FINISH") == "done");
  CHECK(bambu_pretty_status("FAILED") == "failed");
  CHECK(bambu_pretty_status("PAUSE") == "paused");
  CHECK(bambu_pretty_status("PREPARE") == "preparing");
  CHECK(bambu_pretty_status("RUNNING") == "printing");
  CHECK(bambu_pretty_status("OFFLINE") == "offline");
  CHECK(bambu_pretty_status("IDLE") == "idle");
  CHECK(bambu_pretty_status("UNKNOWN").empty());
  CHECK(bambu_pretty_status("").empty());
}

TEST_CASE("bambu_default_stage_label_for_status") {
  CHECK(bambu_default_stage_label_for_status("RUNNING", false) == "Printing");
  CHECK(bambu_default_stage_label_for_status("Downloading", false) == "Model Download");
  CHECK(bambu_default_stage_label_for_status("PAUSE", false) == "Paused");
  CHECK(bambu_default_stage_label_for_status("PREPARE", false) == "Preparing");
  CHECK(bambu_default_stage_label_for_status("FINISH", false) == "Finished");
  CHECK(bambu_default_stage_label_for_status("IDLE", false) == "Idle");
  CHECK(bambu_default_stage_label_for_status("OFFLINE", false) == "Offline");
  CHECK(bambu_default_stage_label_for_status("FAILED", false) == "Stopped");
  CHECK(bambu_default_stage_label_for_status("FAILED", true) == "Failed");
  CHECK(bambu_default_stage_label_for_status("", false) == "Status");
}

TEST_CASE("bambu_stage_label_from_id covers known stages and idle sentinels") {
  CHECK(bambu_stage_label_from_id(0) == "printing");
  CHECK(bambu_stage_label_from_id(2) == "heatbed_preheating");
  CHECK(bambu_stage_label_from_id(4) == "changing_filament");
  CHECK(bambu_stage_label_from_id(7) == "heating_hotend");
  CHECK(bambu_stage_label_from_id(22) == "filament_unloading");
  CHECK(bambu_stage_label_from_id(24) == "filament_loading");
  CHECK(bambu_stage_label_from_id(-1) == "idle");
  CHECK(bambu_stage_label_from_id(255) == "idle");
  CHECK(bambu_stage_label_from_id(99999).empty());
}
