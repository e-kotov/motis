#' Send a request to MOTIS via NNG IPC
#'
#' @param path MOTIS API path, e.g. "/api/v4/plan".
#' @param body A list that will be serialized to JSON as the request body.
#' @param method HTTP method to emulate (default: "POST").
#' @param parse Logical, whether to parse the JSON response.
#' @param timeout The time in milliseconds to wait for a response from the server.
#'   Defaults to 5000 (5 seconds). A value of -1 means wait indefinitely.
#'
#' @return Parsed response (if `parse = TRUE`) or the raw JSON string.
#' @export
motis_request <- function(
  path,
  body = list(),
  method = "POST",
  parse = TRUE,
  timeout = 5000
) {
  stopifnot(is.character(path), length(path) == 1L)
  stopifnot(is.character(method), length(method) == 1L)
  sock <- .motis_env$socket
  if (is.null(sock) || !inherits(sock, "nanoSocket")) {
    stop(
      "No active MOTIS IPC connection. Call motis_connect() first.",
      call. = FALSE
    )
  }

  original_timeout <- nanonext::opt(sock, "recv-timeout")
  on.exit(nanonext::opt(sock, "recv-timeout") <- original_timeout, add = TRUE)
  nanonext::opt(sock, "recv-timeout") <- timeout

  method_upper <- toupper(method)
  envelope <- list(
    path = path,
    method = method_upper,
    body = body
  )

  req_json <- jsonlite::toJSON(envelope, auto_unbox = TRUE, null = "null")
  send_ok <- FALSE
  for (attempt in seq_len(5L)) {
    send_result <- nanonext::send(sock, charToRaw(req_json), mode = "raw")
    if (!inherits(send_result, "errorValue")) {
      send_ok <- TRUE
      break
    }
    if (as.integer(send_result) == 8L) { # NNG_EAGAIN / Try Again
      Sys.sleep(0.01 * attempt)
      next
    }
    stop(
      sprintf("Unable to send IPC request (%s)", format_nng_error(send_result)),
      call. = FALSE
    )
  }
  if (!send_ok) {
    stop("Unable to send IPC request after multiple retries (try again)", call. = FALSE)
  }

  res_raw <- tryCatch(
    {
      nanonext::recv(sock, mode = "raw")
    },
    nanonext_error = function(e) {
      if (grepl("Resource temporarily unavailable", e$message, fixed = TRUE)) {
        return(NULL)
      } else {
        stop(e)
      }
    }
  )

  if (is.null(res_raw)) {
    stop(paste0("Request timed out after ", timeout, "ms."), call. = FALSE)
  }
  if (inherits(res_raw, "errorValue")) {
    stop(
      sprintf("IPC receive failed (%s)", format_nng_error(res_raw)),
      call. = FALSE
    )
  }
  if (!is.raw(res_raw) || length(res_raw) == 0) {
    stop(
      "Received an empty or invalid response from the MOTIS server.",
      call. = FALSE
    )
  }

  res_json <- rawToChar(res_raw)
  if (!parse) {
    return(res_json)
  }

  if (!parse) {
    return(res_json)
  }

  parsed <- tryCatch(
    jsonlite::fromJSON(res_json, simplifyVector = FALSE),
    error = function(e) {
      warning(
        "Failed to parse MOTIS response as JSON; returning raw text.",
        call. = FALSE
      )
      res_json
    }
  )

  parsed
}

format_nng_error <- function(err) {
  code <- as.integer(err)
  msg <- tryCatch(nanonext::nng_error(code), error = function(...) NULL)
  if (length(msg) && nzchar(msg)) {
    sprintf("%d: %s", code, msg)
  } else {
    as.character(code)
  }
}
