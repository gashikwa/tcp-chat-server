/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */

/* 
 * File:   message.h
 * Author: chensh86
 *
 * Created on March 8, 2019, 2:34 PM
 */

#ifndef MESSAGE_H
#define MESSAGE_H

#ifdef __cplusplus
extern "C" {
#endif
    
#define OP_FIELD_WIDTH 16
#define USERNAME_FIELD_WIDTH 16

typedef struct {
    char op[16]; //broadcast, unicast, list, exit, error
    char username[16]; //up to 16 chars
    char* content; //no length limit
} message_t;

int decode_message(message_t* message, const char* in);
int encode_message(const message_t* message, char* out);

#ifdef __cplusplus
}
#endif

#endif /* MESSAGE_H */

