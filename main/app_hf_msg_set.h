#ifndef __APP_HF_MSG_SET_H__
#define __APP_HF_MSG_SET_H__

#include <stdbool.h>

#define HF_MSG_ARGS_MAX (8)

// Connection state tracking
extern bool is_connected;
extern bool connection_in_progress;

typedef int (*hf_cmd_handler)(int argn, char **argv);

typedef struct
{
    const char *str;
    hf_cmd_handler handler;
} hf_msg_hdl_t;

typedef enum hf_cmd_idx
{
    HF_CMD_IDX_CON = 0, /*set up connection with peer device*/
    HF_CMD_IDX_DIS,     /*disconnection with peer device*/
    HF_CMD_IDX_SCAN,    /*scan for Bluetooth devices*/
    HF_CMD_IDX_RESET,   /*reset connection state*/
    HF_CMD_IDX_CONA,    /*set up audio connection with peer device*/
    HF_CMD_IDX_DISA,    /*release audio connection with peer device*/
    HF_CMD_IDX_VU,      /*volume update*/
    HF_CMD_IDX_CIEV,    /*unsolicited indication device status to HF Client*/
    HF_CMD_IDX_VRON,    /*start voice recognition*/
    HF_CMD_IDX_VROFF,   /*stop voice recognition*/
    HF_CMD_IDX_ATE,     /*send extended AT error code*/
    HF_CMD_IDX_IRON,    /*in-band ring tone provided*/
    HF_CMD_IDX_IROFF,   /*in-band ring tone not provided*/
    HF_CMD_IDX_AC,      /*Answer Incoming Call from AG*/
    HF_CMD_IDX_RC,      /*Reject Incoming Call from AG*/
    HF_CMD_IDX_END,     /*End up a call by AG*/
    HF_CMD_IDX_DN       /*Dial Number by AG, e.g. d 11223344*/
} hf_cmd_idx_t;

void register_hfp_ag(void);
#endif /* __APP_HF_MSG_SET_H__*/
