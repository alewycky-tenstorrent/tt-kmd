#include "device.h"
#include "chardev_private.h"
#include "smcmsg.h"

void pump_smc_message_queue(struct tenstorrent_device *tt_dev)
{
	struct chardev_private *priv;
	struct smc_message msg;

	mutex_lock(&tt_dev->smc_message_queue_mutex);

	// If HW has a response, pop it into the first waiting fd.
	if (tt_dev->device_class->smc_get_response(tt_dev, &msg)) {
		if (!tt_dev->smc_message_abandoned) {
			priv = list_first_entry(&tt_dev->smc_message_queue, struct chardev_private, smc_message_queue);

			mutex_lock(&priv->mutex);
			// XXX This could race against an abandon from the same fd

			priv->smc_message = msg;
			priv->smc_message_state = SMC_MESSAGE_STATE_RESPONSE_PRESENT;
			list_del_init(&priv->smc_message_queue);
			mutex_unlock(&priv->mutex);
		}

		tt_dev->smc_message_abandoned = false;
	}

	// If the HW request queue is empty, try to insert a message from a waiting fd.
	if (!list_empty(&tt_dev->smc_message_queue) && tt_dev->device_class->smc_request_queue_empty(tt_dev)) {
		priv = list_first_entry(&tt_dev->smc_message_queue, struct chardev_private, smc_message_queue);
		mutex_lock(&priv->mutex);
		tt_dev->device_class->smc_post_request(tt_dev, &priv->smc_message);
		priv->smc_message_state = SMC_MESSAGE_STATE_REQUEST_IN_FW;
		mutex_unlock(&priv->mutex);
	}

	mutex_unlock(&tt_dev->smc_message_queue_mutex);
}
