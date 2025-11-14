test_that("connects and performs a request", {
  skip_if_not_installed("nanonext")
  skip_if(Sys.getenv("MOTIS_IPC_TEST", "0") != "1",
          "Set MOTIS_IPC_TEST=1 to run IPC integration tests")

  motis_connect()
  on.exit(motis_disconnect(), add = TRUE)

  res <- motis_request("/metrics", body = list(), method = "GET")
  expect_true(is.list(res))
})
