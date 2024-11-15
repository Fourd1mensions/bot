include(cmake/CPM.cmake)

function(SetupDependencies)

  if(NOT TARGET fmtlib::fmtlib)
    CPMAddPackage("gh:fmtlib/fmt#11.0.2")
  endif()

  if(NOT TARGET spdlog::spdlog)
    CPMAddPackage(
      NAME
      spdlog
      VERSION
      1.14.0
      GITHUB_REPOSITORY
      "gabime/spdlog"
      OPTIONS
      "SPDLOG_FMT_EXTERNAL ON"
    )
  endif()

  if (NOT TARGET dpp)
    CPMAddPackage(
      NAME
      dpp
      VERSION
      10.0.30
      GITHUB_REPOSITORY
      "brainboxdotcc/DPP"
      OPTIONS "DPP_CORO ON"
    )
  endif()

  if (NOT TARGET cpr::cpr)
  CPMAddPackage(
    NAME
    cpr
    GIT_TAG 3b15fa82ea74739b574d705fea44959b58142eb8
    GITHUB_REPOSITORY
    "libcpr/cpr"
    OPTIONS
    "CPR_BUILD_TESTS OFF"
    "CPR_USE_SYSTEM_CURL ON"
  )
endif()

endfunction()
