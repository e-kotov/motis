#' Sends a correctly formatted 'hello' request to the MOTIS server.
#' @export
motis_hello_ipc <- function() {
  # The MOTIS IPC message requires the 'content' to be a JSON *string*.
  # We create an empty JSON object string for the content: "{}"
  hello_content_string <- jsonlite::toJSON(list(), auto_unbox = TRUE)

  # This is the correct message envelope.
  message_to_send <- list(
    destination = list(
      type = "Module",
      target = "/"
    ),
    content_type = "MotisNoMessage",
    content = hello_content_string # Note: content is now a string
  )

  sock <- .motis_env$socket
  if (is.null(sock)) {
    stop("Not connected. Call motis_connect() first.")
  }

  req_json <- jsonlite::toJSON(message_to_send, auto_unbox = TRUE)
  nanonext::send(sock, charToRaw(req_json))

  # Use a short timeout
  nanonext::opt(sock, "recv-timeout") <- 2000
  res_raw <- try(nanonext::recv(sock, mode = "raw"), silent = TRUE)

  if (
    inherits(res_raw, "try-error") || !is.raw(res_raw) || length(res_raw) == 0
  ) {
    message("The 'hello' request failed.")
    dput(res_raw)
    return(invisible(NULL))
  }

  message("The 'hello' request SUCCEEDED!")
  res_json <- rawToChar(res_raw)
  return(jsonlite::fromJSON(res_json))
}
