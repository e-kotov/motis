#' Default IPC address helper
#'
#' @return Default IPC address matching the MOTIS server default.
#' @keywords internal
motis_default_address <- function() {
  if (.Platform$OS.type == "windows") {
    "ipc://motis-ipc"
  } else {
    "ipc:///tmp/motis-ipc.sock"
  }
}

# internal state ----
.motis_env <- new.env(parent = emptyenv())
.motis_env$socket <- NULL

#' Connect to a local MOTIS IPC endpoint
#'
#' @param address NNG IPC address, e.g. "ipc:///tmp/motis-ipc.sock".
#'
#' @return Invisibly returns the underlying nanonext socket.
#' @export
motis_connect <- function(address = getOption("motis.nng.address", motis_default_address())) {
  if (!is.null(.motis_env$socket)) {
    motis_disconnect()
  }
  sock <- nanonext::socket("req", dial = address)
  on.exit(
    if (!is.null(sock) && inherits(sock, "nanoSocket")) {
      try(nanonext::close(sock), silent = TRUE)
    },
    add = TRUE,
    after = FALSE
  )
  .motis_env$socket <- sock
  on.exit(NULL, add = FALSE)
  invisible(sock)
}

#' Disconnect from the MOTIS IPC endpoint
#'
#' @export
motis_disconnect <- function() {
  if (!is.null(.motis_env$socket)) {
    try(nanonext::close(.motis_env$socket), silent = TRUE)
    .motis_env$socket <- NULL
  }
  invisible(TRUE)
}
