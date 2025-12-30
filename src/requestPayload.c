#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include "requestPayload.h"
#include "parse_message.h"   

// int parse_request_payload(ParsedMessage *msg, RequestPayload *req)
// {
//     if (!msg || msg->id != 6 || msg->payload_len != 13)
//         return -1;

//     memcpy(&req->index,  msg->payload,      4);
//     memcpy(&req->begin,  msg->payload + 4,  4);
//     memcpy(&req->length, msg->payload + 8,  4);

//     req->index  = ntohl(req->index);
//     req->begin  = ntohl(req->begin);
//     req->length = ntohl(req->length);

//     return 0;
// }
