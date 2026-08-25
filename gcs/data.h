#ifndef CUBESAT_DATA_H
#define CUBESAT_DATA_H

#include "MAVLink.h"

typedef struct {
    MavlinkConnection connection;
    MavlinkTelemetry telemetry;
} DataReader;

int data_reader_open(DataReader *reader, const char *device, int baud);
int data_reader_poll(DataReader *reader, int timeout_ms);
const MavlinkTelemetry *data_reader_telemetry(const DataReader *reader);
void data_reader_close(DataReader *reader);

int data_read_loop(const char *device, int baud);

#endif
