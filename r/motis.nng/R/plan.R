# Create this file as R/plan.R in your package

#' Plan a journey using the MOTIS IPC connection
#'
#' @description
#' This function provides a high-level interface to the MOTIS 'plan' API using an
#' active NNG IPC connection. It can plan multiple journeys at once and process
#' the results into user-friendly formats. It is designed to work with various
#' location formats, including data frames of coordinates.
#'
#' @param from The origin location(s). Can be a character vector of station
#'   IDs, a data frame/tibble with ID or coordinate columns (e.g., `lon`, `lat`),
#'   an `sf` object with POINT geometry, or a numeric matrix (`lon`, `lat`).
#' @param to The destination location(s). Must be of the same type and
#'   length as `from`.
#' @param time The departure or arrival time. Can be a POSIXct object (like
#'   from `Sys.time()`) or a character string in ISO 8601 format
#'   (e.g., "2025-08-15T15:11:00Z"). Defaults to the current time.
#' @param arrive_by Logical. If `TRUE`, `time` is treated as the arrival time.
#'   Defaults to `FALSE` (departure time).
#' @param from_id_col The name of the column in `from` containing station IDs.
#'   Defaults to `"id"`.
#' @param to_id_col The name of the column in `to` containing station IDs.
#'   Defaults to `"id"`.
#' @param ... Additional arguments passed on to the MOTIS plan API. For example,
#'   `maxTransfers = 0` or `directModes = "WALK"`. See the MOTIS API
#'   documentation for all available parameters.
#' @param output The desired output format. One of:
#'   - `"itineraries"` (default): An `sf` data frame of itineraries, with combined
#'     line geometry for the entire trip.
#'   - `"legs"`: An `sf` data frame of individual journey legs, with line
#'     geometry for each leg.
#'   - `"raw_list"`: A list of the raw parsed JSON responses from the server.
#' @return Depending on the `output` parameter, an `sf` data frame or a list.
#' @export
#' @examples
#' \dontrun{
#' # Ensure you have an active connection first
#' motis_connect()
#'
#' # --- Example using data frames with lon/lat coordinates ---
#' from_location <- data.frame(lon = 12.58134, lat = 55.62361)
#' to_location <- data.frame(lon = 12.51633, lat = 55.83582)
#'
#' # Plan a trip and get the legs as an sf data frame
#' trip_legs <- motis_plan_ipc(
#'   from = from_location,
#'   to = to_location,
#'   output = "legs"
#' )
#'
#' print(trip_legs)
#'
#' # Get the full itineraries instead
#' trip_itineraries <- motis_plan_ipc(
#'   from = from_location,
#'   to = to_location,
#'   output = "itineraries"
#' )
#'
#' print(trip_itineraries)
#'
#' # Disconnect when done
#' motis_disconnect()
#' }
motis_plan_ipc <- function(
  from,
  to,
  time = Sys.time(),
  arrive_by = FALSE,
  from_id_col = "id",
  to_id_col = "id",
  ...,
  output = c("itineraries", "legs", "raw_list")
) {
  # --- 1. Argument and Input Validation ---
  output <- match.arg(output)
  if (NROW(from) != NROW(to)) {
    stop("Length of 'from' and 'to' must be equal.", call. = FALSE)
  }

  # --- 2. Format Inputs ---
  from_place <- .format_place(from, id_col = from_id_col)
  to_place <- .format_place(to, id_col = to_id_col)

  # --- 3. Build Request Bodies ---
  time_str <- format(
    as.POSIXct(time, tz = "UTC"),
    "%Y-%m-%dT%H:%M:%SZ",
    tz = "UTC"
  )
  dots <- list(...)

  make_body <- function(from, to) {
    body <- c(
      list(
        fromPlace = from,
        toPlace = to,
        time = time_str,
        arriveBy = isTRUE(arrive_by)
      ),
      dots
    )
    # Convert multi-element vectors to comma-separated strings for the API
    lapply(body, function(x) if (length(x) > 1) paste(x, collapse = ",") else x)
  }

  request_bodies <- mapply(make_body, from_place, to_place, SIMPLIFY = FALSE)

  # --- 4. Perform Requests Sequentially ---
  # The req/rep socket pattern is synchronous, so we send and receive one by one.
  parsed_responses <- lapply(request_bodies, function(body) {
    motis_request(
      path = "/api/v4/plan",
      body = body,
      method = "POST",
      parse = TRUE
    )
  })

  # --- 5. Process and Parse Responses ---
  if (output == "raw_list") {
    return(parsed_responses)
  }

  names(parsed_responses) <- seq_along(parsed_responses)

  if (output == "legs") {
    return(purrr::list_rbind(
      lapply(
        parsed_responses,
        .flatten_legs,
        decode_geom = TRUE,
        include_direct = TRUE
      ),
      names_to = "request_id"
    ))
  }

  if (output == "itineraries") {
    itineraries <- purrr::list_rbind(
      lapply(parsed_responses, .flatten_itineraries, include_direct = TRUE),
      names_to = "request_id"
    )
    if (nrow(itineraries) == 0) {
      return(sf::st_as_sf(itineraries))
    }

    legs <- purrr::list_rbind(
      lapply(
        parsed_responses,
        .flatten_legs,
        decode_geom = TRUE,
        include_direct = TRUE
      ),
      names_to = "request_id"
    )

    if (
      nrow(legs) > 0 &&
        "geom" %in% names(legs) &&
        requireNamespace("sf", quietly = TRUE)
    ) {
      # Aggregate leg geometries to create a single geometry for each itinerary
      itinerary_geoms <- stats::aggregate(
        legs$geom,
        by = list(
          request_id = legs$request_id,
          kind = legs$kind,
          itin_id = legs$itin_id
        ),
        FUN = sf::st_combine
      )
      names(itinerary_geoms)[names(itinerary_geoms) == "x"] <- "geom"

      itineraries <- merge(
        itineraries,
        itinerary_geoms,
        by = c("request_id", "kind", "itin_id"),
        all.x = TRUE
      )
    }

    return(sf::st_as_sf(itineraries))
  }
}


# -------------------------------------------------------------------------
# Helper functions (adapted from the rmotis package)
# -------------------------------------------------------------------------

`%||%` <- function(x, y) if (!is.null(x)) x else y

.flatten_itineraries <- function(res, include_direct = TRUE) {
  build <- function(itins, kind_label) {
    if (length(itins) == 0) {
      return(NULL)
    }
    do.call(
      rbind,
      lapply(seq_along(itins), function(i) {
        it <- itins[[i]]
        data.frame(
          kind = kind_label,
          itin_id = i,
          duration = it$duration %||% NA_real_,
          startTime = it$startTime %||% NA_character_,
          endTime = it$endTime %||% NA_character_,
          transfers = it$transfers %||% NA_integer_
        )
      })
    )
  }
  dplyr::bind_rows(
    build(res$itineraries, "itinerary"),
    if (isTRUE(include_direct)) build(res$direct, "direct")
  )
}

.flatten_legs <- function(res, decode_geom = FALSE, include_direct = TRUE) {
  process_itineraries <- function(itins, kind_label) {
    if (length(itins) == 0) {
      return(NULL)
    }

    all_legs_nested <- lapply(seq_along(itins), function(i) {
      legs <- itins[[i]]$legs
      if (length(legs) == 0) {
        return(NULL)
      }

      df <- do.call(
        rbind,
        lapply(seq_along(legs), function(j) {
          l <- legs[[j]]
          data.frame(
            itin_id = i,
            leg_index = j,
            mode = l$mode %||% NA_character_,
            from_name = l$from$name %||% NA_character_,
            to_name = l$to$name %||% NA_character_,
            startTime = l$startTime %||% l$from$departure %||% NA_character_,
            endTime = l$endTime %||% l$to$arrival %||% NA_character_,
            distance = l$distance %||% NA_real_
          )
        })
      )

      if (isTRUE(decode_geom)) {
        polylines <- vapply(
          legs,
          function(l) l$legGeometry$points %||% l$polyline %||% NA_character_,
          CHARACTER
        )
        df$geom <- .decode_polylines_to_sfc(polylines)
      }
      df
    })

    df_all <- do.call(rbind, all_legs_nested)
    if (is.null(df_all)) {
      return(NULL)
    }

    df_all$kind <- kind_label
    df_all
  }

  df <- dplyr::bind_rows(
    process_itineraries(res$itineraries, "itinerary"),
    if (isTRUE(include_direct)) process_itineraries(res$direct, "direct")
  )

  if (is.null(df) || nrow(df) == 0) {
    return(sf::st_sf(data.frame()))
  }

  if (isTRUE(decode_geom)) sf::st_as_sf(df) else df
}

.decode_polylines_to_sfc <- function(polylines) {
  if (
    !requireNamespace("googlePolylines", quietly = TRUE) ||
      !requireNamespace("sf", quietly = TRUE)
  ) {
    warning(
      "Install 'googlePolylines' and 'sf' to decode geometries.",
      call. = FALSE
    )
    return(sf::st_sfc(
      lapply(polylines, function(x) sf::st_linestring()),
      crs = 4326
    ))
  }

  sfc <- lapply(polylines, function(p) {
    if (is.na(p) || !nzchar(p)) {
      return(sf::st_linestring())
    }

    coords <- googlePolylines::decode(p)[[1]]
    if (is.null(coords) || nrow(coords) < 2) {
      return(sf::st_linestring())
    }

    # googlePolylines returns lat/lon, st_linestring expects x/y (lon/lat)
    sf::st_linestring(as.matrix(coords[, c("lon", "lat")]))
  })

  sf::st_sfc(sfc, crs = 4326)
}

.format_place <- function(place, id_col = "id") {
  if (inherits(place, "sf")) {
    coords <- sf::st_coordinates(place)
    return(paste(round(coords[, "Y"], 6), round(coords[, "X"], 6), sep = ","))
  }

  if (is.data.frame(place)) {
    p_names <- tolower(names(place))
    id_col_lower <- tolower(id_col)

    if (id_col_lower %in% p_names) {
      return(as.character(place[[which(p_names == id_col_lower)]]))
    }

    lat_col <- which(p_names %in% c("lat", "latitude"))
    lon_col <- which(p_names %in% c("lon", "lng", "longitude"))

    if (length(lat_col) == 1 && length(lon_col) == 1) {
      return(paste(place[[lat_col]], place[[lon_col]], sep = ","))
    }
    stop(
      "Data frame must contain either an '",
      id_col,
      "' column or coordinate columns ('lat', 'lon').",
      call. = FALSE
    )
  }

  if (is.matrix(place) && is.numeric(place)) {
    if (ncol(place) != 2) {
      stop("Matrix must have 2 columns.", call. = FALSE)
    }
    # Assume [lon, lat] column order, return "lat,lon" string
    return(paste(place[, 2], place[, 1], sep = ","))
  }

  if (is.character(place)) {
    return(place)
  }

  stop(
    "Unsupported input type for 'from'/'to'. Must be sf, data.frame, matrix, or character.",
    call. = FALSE
  )
}
