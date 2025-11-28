#include <linux/types.h>

struct tenstorrent_device;

// This could be a request or a response.
struct smc_message
{
	u32 data[8];
};

enum smc_message_state {
	SMC_MESSAGE_STATE_EMPTY,
	SMC_MESSAGE_STATE_REQUEST_PRESENT,
    SMC_MESSAGE_STATE_REQUEST_IN_FW,
	SMC_MESSAGE_STATE_RESPONSE_PRESENT,
};

void pump_smc_message_queue(struct tenstorrent_device *tt_dev);
