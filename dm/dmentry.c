/*
 *	This program is free software: you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation, either version 2 of the License, or
 *	(at your option) any later version.
 *
 *	Copyright (C) 2015 Inteno Broadband Technology AB
 *	  Author MOHAMED Kallel <mohamed.kallel@pivasoftware.com>
 *	  Author Imen Bhiri <imen.bhiri@pivasoftware.com>
 *	  Author Feten Besbes <feten.besbes@pivasoftware.com>
 *
 */

#include "dmcwmp.h"
#include "dmubus.h"
#include "dmuci.h"
#include "dmentry.h"

LIST_HEAD(head_package_change);

int dm_global_init(void)
{
	memset(&dmubus_ctx, 0, sizeof(struct dmubus_ctx));
	INIT_LIST_HEAD(&dmubus_ctx.obj_head);
	uci_ctx = uci_alloc_context();
	return 0;
}

int dm_global_clean(void)
{
	if (uci_ctx) uci_free_context(uci_ctx);
	uci_ctx= NULL;
	dmubus_ctx_free(&dmubus_ctx);
	dmcleanmem();
	return 0;
}

int dm_ctx_init(struct dmctx *ctx)
{
	INIT_LIST_HEAD(&ctx->list_parameter);
	INIT_LIST_HEAD(&ctx->set_list_tmp);
	INIT_LIST_HEAD(&ctx->list_fault_param);
	return 0;
}

int dm_ctx_clean(struct dmctx *ctx)
{
	free_all_list_parameter(ctx);
	free_all_set_list_tmp(ctx);
	free_all_list_fault_param(ctx);
	DMFREE(ctx->addobj_instance);
	return 0;
}

int dm_entry_param_method(struct dmctx *ctx, int cmd, char *inparam, char *arg1, char *arg2)
{
	int fault = 0;
	bool setnotif = true;
	
	if (!inparam) inparam = "";
	ctx->in_param = inparam;

	if (ctx->in_param[0] == '\0' || strcmp(ctx->in_param, DMROOT) == 0) {
		ctx->tree = true;
	} else {
		ctx->tree = false;
	}

	switch(cmd) {
		case CMD_GET_VALUE:
			fault = dm_entry_get_value(ctx);
			break;
		case CMD_GET_NAME:
			if (arg1 && string_to_bool(arg1, &ctx->nextlevel) == 0){
				fault = dm_entry_get_name(ctx);
			} else {
				fault = FAULT_9003;
			}
			break;
		case CMD_GET_NOTIFICATION:
			fault = dm_entry_get_notification(ctx);
			break;
		case CMD_SET_VALUE:
			ctx->in_value = arg1 ? arg1 : "";
			ctx->setaction = VALUECHECK;
			fault = dm_entry_set_value(ctx);
			if (fault)
				add_list_fault_param(ctx, ctx->in_param, fault);
			break;
		case CMD_SET_NOTIFICATION:
			if (arg2)
				string_to_bool(arg2, &setnotif);
			if (setnotif && arg1 &&
				(strcmp(arg1, "0") == 0 ||
				strcmp(arg1, "1") == 0  ||
				strcmp(arg1, "2") == 0)) {
				ctx->in_notification = arg1;
				ctx->setaction = VALUECHECK;
				fault = dm_entry_set_notification(ctx);
			} else {
				fault = FAULT_9003;
			}
			break;
		case CMD_INFORM:
			dm_entry_inform(ctx);
			break;
		case CMD_ADD_OBJECT:
			fault = dm_entry_add_object(ctx);
			if (!fault) {
				dmuci_set_value("cwmp", "acs", "ParameterKey", arg1 ? arg1 : "");
			}
			break;
		case CMD_DEL_OBJECT:
			fault = dm_entry_delete_object(ctx);
			if (!fault) {
				dmuci_set_value("cwmp", "acs", "ParameterKey", arg1 ? arg1 : "");
			}
			break;
	}
	dmuci_commit();
	return fault;
}

int dm_entry_apply(struct dmctx *ctx, int cmd, char *arg1, char *arg2)
{
	int fault = 0;
	struct set_tmp *n, *p;
	
	switch(cmd) {
		case CMD_SET_VALUE:
			ctx->setaction = VALUESET;
			ctx->tree = false;
			list_for_each_entry_safe(n, p, &ctx->set_list_tmp, list) {
				ctx->in_param = n->name;
				ctx->in_value = n->value ? n->value : "";
				fault = dm_entry_set_value(ctx);
				del_set_list_tmp(n);
				if (fault) break;
			}
			if (fault) {
				//Should not happen
				dmuci_revert();
				add_list_fault_param(ctx, ctx->in_param, fault);
			} else {
				dmuci_set_value("cwmp", "acs", "ParameterKey", arg1 ? arg1 : "");
				dmuci_change_packages(&head_package_change);
				dmuci_commit();
			}
			break;
		case CMD_SET_NOTIFICATION:
			ctx->setaction = VALUESET;
			ctx->tree = false;
			list_for_each_entry_safe(n, p, &ctx->set_list_tmp, list) {
				ctx->in_param = n->name;
				ctx->in_notification = n->value ? n->value : "0";
				fault = dm_entry_set_notification(ctx);
				del_set_list_tmp(n);
				if (fault) break;
			}
			if (fault) {
				//Should not happen
				dmuci_revert();
			} else {
				dmuci_commit();
			}
			break;
	}
	return fault;
}

int cli_output_dm_result(struct dmctx *dmctx, int fault, int cmd, int out)
{
	if (!out) return 0;

	if (dmctx->list_fault_param.next != &dmctx->list_fault_param) {
		struct param_fault *p;
		list_for_each_entry(p, &dmctx->list_fault_param, list) {
			fprintf (stdout, "{ \"parameter\": \"%s\", \"fault\": \"%d\" }\n", p->name, p->fault);
		}
		goto end;
	}
	if (fault) {
		fprintf (stdout, "{ \"fault\": \"%d\" }\n", fault);
		goto end;
	}

	if (cmd == CMD_ADD_OBJECT) {
		if (dmctx->addobj_instance) {
			fprintf (stdout, "{ \"status\": \"1\", \"instance\": \"%s\" }\n", dmctx->addobj_instance);
			goto end;
		} else {
			fprintf (stdout, "{ \"fault\": \"%d\" }\n", FAULT_9002);
			goto end;
		}
	}

	if (cmd == CMD_DEL_OBJECT || cmd == CMD_SET_VALUE) {
		fprintf (stdout, "{ \"status\": \"1\" }\n");
		goto end;
	}

	if (cmd == CMD_SET_NOTIFICATION) {
		fprintf (stdout, "{ \"status\": \"0\" }\n");
		goto end;
	}

	struct dm_parameter *n;
	if (cmd == CMD_GET_NAME) {
		list_for_each_entry(n, &dmctx->list_parameter, list) {
			fprintf (stdout, "{ \"parameter\": \"%s\", \"writable\": \"%s\" }\n", n->name, n->data);
		}
	}
	else if (cmd == CMD_GET_NOTIFICATION) {
		list_for_each_entry(n, &dmctx->list_parameter, list) {
			fprintf (stdout, "{ \"parameter\": \"%s\", \"notification\": \"%s\" }\n", n->name, n->data);
		}
	}
	else if (cmd == CMD_GET_VALUE || cmd == CMD_INFORM) {
		list_for_each_entry(n, &dmctx->list_parameter, list) {
			fprintf (stdout, "{ \"parameter\": \"%s\", \"value\": \"%s\", \"type\": \"%s\" }\n", n->name, n->data, n->type);
		}
	}
end:
	return 0;
}

void dm_entry_cli(int argc, char** argv)
{
	struct dmctx cli_dmctx = {0};
	int output = 1;

	dm_global_init();
	dm_ctx_init(&cli_dmctx);

	if (argc < 4) goto invalid_arguments;

	output = atoi(argv[2]);
	char *cmd = argv[3];

	char *param, *next_level, *parameter_key, *value;
	int fault = 0, status = -1;

	/* GET NAME */
	if (strcmp(cmd, "get_name") == 0) {
		if (argc < 6) goto invalid_arguments;
		param =argv[4];
		next_level =argv[5];
		fault = dm_entry_param_method(&cli_dmctx, CMD_GET_NAME, param, next_level, NULL);
		cli_output_dm_result(&cli_dmctx, fault, CMD_GET_NAME, output);
	}
	/* GET VALUE */
	else if (strcmp(cmd, "get_value") == 0) {
		if (argc < 5) goto invalid_arguments;
		param =argv[4];
		fault = dm_entry_param_method(&cli_dmctx, CMD_GET_VALUE, param, NULL, NULL);
		cli_output_dm_result(&cli_dmctx, fault, CMD_GET_VALUE, output);
	}
	/* GET NOTIFICATION */
	else if (strcmp(cmd, "get_notification") == 0) {
		if (argc < 5) goto invalid_arguments;
		param =argv[4];
		fault = dm_entry_param_method(&cli_dmctx, CMD_GET_NOTIFICATION, param, NULL, NULL);
		cli_output_dm_result(&cli_dmctx, fault, CMD_GET_NOTIFICATION, output);
	}
	/* SET VALUE */
	else if (strcmp(cmd, "set_value") == 0) {
		if (argc < 7 || (argc % 2) == 0) goto invalid_arguments;
		int i;
		for (i=5; i<argc; i++) {
			param = argv[i];
			value = argv[i+1];
			dm_entry_param_method(&cli_dmctx, CMD_SET_VALUE, param, value, NULL);
		}
		parameter_key = argv[4];
		fault = dm_entry_apply(&cli_dmctx, CMD_SET_VALUE, parameter_key, NULL);
		cli_output_dm_result(&cli_dmctx, fault, CMD_SET_VALUE, output);
	}
	/* SET NOTIFICATION */
	else if (strcmp(cmd, "set_notification") == 0) {
		if (argc < 6 || (argc % 2) != 0) goto invalid_arguments;
		int i;
		for (i=4; i<argc; i++) {
			param = argv[i];
			value = argv[i+1];
			dm_entry_param_method(&cli_dmctx, CMD_SET_NOTIFICATION, param, value, "1");
		}
		fault = dm_entry_apply(&cli_dmctx, CMD_SET_NOTIFICATION, NULL, NULL);
		cli_output_dm_result(&cli_dmctx, fault, CMD_SET_NOTIFICATION, output);
	}
	/* ADD OBJECT */
	else if (strcmp(cmd, "add_object") == 0) {
		if (argc < 6) goto invalid_arguments;
		param =argv[5];
		parameter_key =argv[4];
		fault = dm_entry_param_method(&cli_dmctx, CMD_ADD_OBJECT, param, parameter_key, NULL);
		cli_output_dm_result(&cli_dmctx, fault, CMD_ADD_OBJECT, output);
	}
	/* DEL OBJECT */
	else if (strcmp(cmd, "delete_object") == 0) {
		if (argc < 6) goto invalid_arguments;
		param =argv[5];
		parameter_key =argv[4];
		fault = dm_entry_param_method(&cli_dmctx, CMD_DEL_OBJECT, param, parameter_key, NULL);
		cli_output_dm_result(&cli_dmctx, fault, CMD_DEL_OBJECT, output);
	}
	/* INFORM */
	else if (strcmp(cmd, "inform") == 0) {
		fault = dm_entry_param_method(&cli_dmctx, CMD_INFORM, "", NULL, NULL);
		cli_output_dm_result(&cli_dmctx, fault, CMD_INFORM, output);
	}
	else {
		goto invalid_arguments;
	}
	dm_ctx_clean(&cli_dmctx);
	dm_global_clean();
	return;

invalid_arguments:
	dm_ctx_clean(&cli_dmctx);
	dm_global_clean();
	fprintf(stdout, "Invalid arguments!\n");;
}

int cli_output_wepkey64(char strk64[4][11])
{
	fprintf(stdout, "%s\n%s\n%s\n%s\n", strk64[0], strk64[1], strk64[2], strk64[3]);
	return 0;
}

int cli_output_wepkey128(char strk128[27])
{
	fprintf(stdout, "%s\n", strk128);
	return 0;
}

void wepkey_cli(int argc, char** argv)
{
	if (argc < 4) goto invalid_arguments;

	char *strength = argv[2];
	char *passphrase =  argv[3];

	if (!strength || !passphrase || passphrase[0] == '\0')
		goto invalid_arguments;

	if (strcmp(strength, "64") == 0) {
		char strk64[4][11];
		wepkey64(passphrase, strk64);
		cli_output_wepkey64(strk64);
	}
	else if (strcmp(strength, "128") == 0) {
		char strk128[27];
		wepkey128(passphrase, strk128);
		cli_output_wepkey128(strk128);
	}
	else {
		goto invalid_arguments;
	}
	return;

invalid_arguments:
	fprintf(stdout, "Invalid arguments!\n");;
}