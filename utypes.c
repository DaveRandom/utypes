/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2017 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: krakjoe                                                      |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_utypes.h"

#if PHP_VERSION_ID < 70200
#define PHP_UTYPE(t)				 (t)
#define PHP_UTYPE_CODE(t)			 (t).type_hint
#define PHP_UTYPE_NAME(t)			 (t).class_name
#define PHP_UTYPE_IS_NULLABLE(t)	 (t).allow_null
#define PHP_UTYPE_IS_CLASS(t) 		 (PHP_UTYPE_CODE(t) == IS_OBJECT)
#elif PHP_VERSION_ID >= 70200
#define PHP_UTYPE(t)				 (t).type
#define PHP_UTYPE_CODE(t)			 ZEND_TYPE_CODE(PHP_UTYPE(t))
#define PHP_UTYPE_NAME(t)			 ZEND_TYPE_NAME(PHP_UTYPE(t))
#define PHP_UTYPE_IS_NULLABLE(t)	 ZEND_TYPE_ALLOW_NULL(PHP_UTYPE(t))
#define PHP_UTYPE_IS_CLASS(t)		 ZEND_TYPE_IS_CLASS(PHP_UTYPE(t))
#endif

#define PHP_UTYPE_CODE_MATCH(a, b)	 ZEND_SAME_FAKE_TYPE(PHP_UTYPE_CODE(a), PHP_UTYPE_CODE(b))

#define PHP_UTYPES_HANDLER_FLAG_PARAM      (1 << 0)
#define PHP_UTYPES_HANDLER_FLAG_RETURN     (1 << 1)
#define PHP_UTYPES_HANDLER_FLAG_INSTANCEOF (1 << 2)
#define PHP_UTYPES_HANDLER_FLAG_NULLABLE   (1 << 3)
#define PHP_UTYPES_HANDLER_FLAG_VARIADIC   (1 << 4)

struct _php_utypes_handler {
	zval zv;
	zend_fcall_info fci;
	zend_fcall_info_cache fcc;
};

ZEND_DECLARE_MODULE_GLOBALS(utypes);

/* {{{ */
static inline void php_utypes_handler_ctor(php_utypes_handler *handler)
{
	ZVAL_NULL(&handler->zv);
	handler->fci = empty_fcall_info;
	handler->fcc = empty_fcall_info_cache;
}
/* }}} */

#define php_utypes_handler_dtor(handler) php_utypes_replace_handler((handler), NULL, 1)

/* {{{ */
static int php_utypes_handler_set(php_utypes_handler *handler, zval *znewfn, zend_bool allow_null)
{
	zend_fcall_info fci = empty_fcall_info;
	zend_fcall_info_cache fcc = empty_fcall_info_cache;

	if (!znewfn || Z_ISNULL(znewfn)) {
		if (!allow_null) {
			return FAILURE;
		}

		znewfn = NULL;
	} else if (zend_fcall_info_init(znewfn, IS_CALLABLE_CHECK_SILENT, &fci, &fcc, NULL, NULL) != SUCCESS) {
		return FAILURE;
	}

	if (!Z_ISNULL(handler->zv)) {
		zval_ptr_dtor(&handler->zv);
	}

	if (znewfn) {
		ZVAL_COPY(&handler->zv, znewfn);
		memcpy(&handler->fci, &fci, sizeof(zend_fcall_info));
		memcpy(&handler->fcc, &fcc, sizeof(zend_fcall_info_cache));
	} else {
		ZVAL_NULL(&handler->zv);
	}

	return SUCCESS;
}
/* }}} */

/* {{{ proto ?callable utypes\handler( [ ?callable handler ] ) */
PHP_FUNCTION(handler)
{
	zval *callback;

	RETVAL_ZVAL(&UTG(default_handler).zv, 1, 0);

	ZEND_PARSE_PARAMETERS_START(0, 1)
		Z_PARAM_ZVAL_EX(callback, 1, 0)
	ZEND_PARSE_PARAMETERS_END();

	if (ZEND_NUM_ARGS() == 0) {
		php_utypes_handler_set(&UTG(default_handler), callback, 1);
	}
}
/* }}} */

/* {{{ */
static inline void php_utypes_verify(zend_function *handler, zend_function *interface, zval *return_value)
{
	uint32_t arg = 0, end = interface->common.num_args;

	if (interface->common.fn_flags & ZEND_ACC_HAS_RETURN_TYPE) {
		if (!(handler->common.fn_flags & ZEND_ACC_HAS_RETURN_TYPE)) {
			RETURN_FALSE;
		}

		if (PHP_UTYPE_IS_CLASS(handler->common.arg_info[-1])) {
			if (!zend_string_equals_ci(
					PHP_UTYPE_NAME(handler->common.arg_info[-1]),
					PHP_UTYPE_NAME(interface->common.arg_info[-1]))) {
				RETURN_FALSE;
			}
		} else {
			if (!PHP_UTYPE_CODE_MATCH(handler->common.arg_info[-1], interface->common.arg_info[-1])) {
				RETURN_FALSE;
			}
		}
	}

	if (interface->common.fn_flags & ZEND_ACC_RETURN_REFERENCE) {
		if (!(handler->common.fn_flags & ZEND_ACC_RETURN_REFERENCE)) {
			RETURN_FALSE;
		}
	}

	if (interface->common.fn_flags & ZEND_ACC_VARIADIC) {
		if (!(handler->common.fn_flags & ZEND_ACC_VARIADIC)) {
			RETURN_FALSE;
		}
	}

	if (interface->common.required_num_args < handler->common.required_num_args ||
		interface->common.num_args > handler->common.num_args) {
		RETURN_FALSE;
	}

	if (interface->common.fn_flags & ZEND_ACC_VARIADIC) {
		end++;
	}

	if (handler->common.num_args >= interface->common.num_args) {
		end = handler->common.num_args;

		if (handler->common.fn_flags & ZEND_ACC_VARIADIC) {
			end++;
		}
	}

	while (arg < end) {
		if (PHP_UTYPE_IS_CLASS(handler->common.arg_info[arg])) {
			if (!zend_string_equals_ci(
					PHP_UTYPE_NAME(handler->common.arg_info[arg]),
					PHP_UTYPE_NAME(interface->common.arg_info[arg]))) {
				RETURN_FALSE;
			}
		} else {
			if (!PHP_UTYPE_CODE_MATCH(handler->common.arg_info[arg], interface->common.arg_info[arg])) {
				RETURN_FALSE;
			}
		}

		arg++;
	}

	RETURN_TRUE;
} /* }}} */

/* {{{ proto bool utypes\verify(callable function, string interface) */
PHP_FUNCTION(verify)
{
	zend_fcall_info fci = empty_fcall_info;
	zend_fcall_info_cache fcc = empty_fcall_info_cache;
	zend_class_entry *interface = NULL;
	zend_function *function = NULL;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "fC", &fci, &fcc, &interface) != SUCCESS) {
		return;
	}

	if (zend_hash_num_elements(&interface->function_table) != 1) {
		RETURN_FALSE;
	}

	ZEND_HASH_FOREACH_PTR(&interface->function_table, function) {
		break;
	} ZEND_HASH_FOREACH_END();

	if (!function || function->type != ZEND_USER_FUNCTION) {
		return;
	}

	php_utypes_verify(fcc.function_handler, function, return_value);
} /* }}} */

/* {{{ php_utypes_init_globals */
static void php_utypes_init_globals(zend_utypes_globals *utg)
{
	php_utypes_handler_ctor(&utg->default_handler);
	utg->busy = 0;
}
/* }}} */

/* {{{ */
static inline int php_utypes_args(zend_execute_data *execute_data, zend_function *func, const zend_op *opline, zend_fcall_info *fci)
{
	zval name, flags;

	ZVAL_UNDEF(&name);
	ZVAL_LONG(&flags, 0);

	switch (opline->opcode) {
		case ZEND_RECV:
		case ZEND_RECV_INIT: {
			if (!PHP_UTYPE_IS_CLASS(func->common.arg_info[opline->op1.num - 1])) {
				return FAILURE;
			}

			ZVAL_STR(&name, PHP_UTYPE_NAME(func->common.arg_info[opline->op1.num - 1]));

			Z_LVAL(flags) |= PHP_UTYPES_HANDLER_FLAG_PARAM;
			if (PHP_UTYPE_IS_NULLABLE(func->common.arg_info[opline->op1.num - 1])) {
				Z_LVAL(flags) |= PHP_UTYPES_HANDLER_FLAG_NULLABLE;
			}

			zend_fcall_info_argn(fci, 3, &name, ZEND_CALL_ARG(execute_data, opline->op1.num), &flags);

			return SUCCESS;
		}

		case ZEND_RECV_VARIADIC: {
			zval params, *variadic;
			uint32_t n = opline->op1.num, c = ZEND_CALL_NUM_ARGS(execute_data);

			if (!PHP_UTYPE_IS_CLASS(func->common.arg_info[opline->op1.num - 1])) {
				return FAILURE;
			}

			ZVAL_STR(&name, PHP_UTYPE_NAME(func->common.arg_info[opline->op1.num - 1]));

			array_init(&params);

			if (n <= c) {
				variadic = ZEND_CALL_VAR_NUM(execute_data, func->op_array.last_var + func->op_array.T);

				do {
					add_next_index_zval(&params, variadic);
					Z_TRY_ADDREF_P(variadic);
					variadic++;
				} while(++n <= c);
			}

			Z_LVAL(flags) |= PHP_UTYPES_HANDLER_FLAG_PARAM | PHP_UTYPES_HANDLER_FLAG_VARIADIC;
			if (PHP_UTYPE_IS_NULLABLE(func->common.arg_info[opline->op1.num - 1])) {
				Z_LVAL(flags) |= PHP_UTYPES_HANDLER_FLAG_NULLABLE;
			}

			zend_fcall_info_argn(fci, 3, &name, &params, &flags);

			Z_SET_REFCOUNT_P(&params, 1);

			return SUCCESS;
		}

		case ZEND_VERIFY_RETURN_TYPE: {
			zval nil, *value = &nil;

			if (!PHP_UTYPE_IS_CLASS(func->common.arg_info[-1])) {
				return FAILURE;
			}

			ZVAL_STR(&name, PHP_UTYPE_NAME(func->common.arg_info[-1]));

			if (opline->op1_type == IS_CONST) {
				value = EX_CONSTANT(opline->op1);
			} else {
				value = ZEND_CALL_VAR(execute_data, opline->op1.num);

				if (Z_TYPE_P(value) == IS_INDIRECT) {
					value = Z_INDIRECT_P(value);
				}

				if (Z_ISUNDEF_P(value)) {
					ZVAL_NULL(&nil);
				}

				ZVAL_DEREF(value);
			}

			Z_LVAL(flags) |= PHP_UTYPES_HANDLER_FLAG_RETURN;
			if (PHP_UTYPE_IS_NULLABLE(func->common.arg_info[-1])) {
				Z_LVAL(flags) |= PHP_UTYPES_HANDLER_FLAG_NULLABLE;
			}

			zend_fcall_info_argn(fci, 3, &name, value, &flags);

			return SUCCESS;
		}
	}

	return FAILURE;
} /* }}} */

/* {{{ */
static inline zend_bool php_utypes_hints(zend_execute_data *execute_data)
{
	switch (EX(opline)->opcode) {
		case ZEND_RECV:
		case ZEND_RECV_INIT:
		case ZEND_RECV_VARIADIC:
			return (EX(func)->common.fn_flags & ZEND_ACC_HAS_TYPE_HINTS) == ZEND_ACC_HAS_TYPE_HINTS;

		case ZEND_VERIFY_RETURN_TYPE:
			return (EX(func)->common.fn_flags & ZEND_ACC_HAS_RETURN_TYPE) == ZEND_ACC_HAS_RETURN_TYPE;
	}

	return 0;
} /* }}} */

/* {{{ */
static inline zend_bool php_utypes_ready()
{
	return !UTG(busy) || !UTYPES_HAVE_DEFAULT_HANDLER();
} /* }}} */

/* {{{ */
static inline void php_utypes_enter(zval *rv)
{
	UTG(busy) = 1;

	UTG(default_handler).fci.retval = rv;

	ZVAL_UNDEF(UTG(default_handler).fci.retval);
} /* }}} */

/* {{{ */
static inline int php_utypes_leave(int action)
{
	UTG(busy) = 0;

	if (UTG(default_handler).fci.param_count) {
		zend_fcall_info_args_clear(&UTG(default_handler).fci, 1);
	}

	if (!Z_ISUNDEF_P(UTG(default_handler).fci.retval)) {
		zval_ptr_dtor(UTG(default_handler).fci.retval);

		ZVAL_UNDEF(UTG(default_handler).fci.retval);
	}

	return action;
} /* }}} */

/* {{{ */
static int php_utypes_handle_op(zend_execute_data *execute_data)
{
	zval rv;

	if (!php_utypes_ready() || !php_utypes_hints(execute_data)) {
		return ZEND_USER_OPCODE_DISPATCH;
	}

	if (EX(func)->type != ZEND_USER_FUNCTION) {
		return ZEND_USER_OPCODE_DISPATCH;
	}

	php_utypes_enter(&rv);

	if (php_utypes_args(execute_data, EX(func), EX(opline), &UTG(default_handler).fci) != SUCCESS) {
		return php_utypes_leave(ZEND_USER_OPCODE_DISPATCH);
	}

	if (zend_call_function(&UTG(default_handler).fci, &UTG(default_handler).fcc) != SUCCESS) {
		return php_utypes_leave(ZEND_USER_OPCODE_DISPATCH);
	}

	if (zend_is_true(&rv)) {
		EX(opline)++;

		return php_utypes_leave(ZEND_USER_OPCODE_CONTINUE);
	}

	return php_utypes_leave(ZEND_USER_OPCODE_DISPATCH);
} /* }}} */

/* {{{ PHP_MINIT_FUNCTION */
PHP_MINIT_FUNCTION(utypes)
{
	ZEND_INIT_MODULE_GLOBALS(utypes, php_utypes_init_globals, NULL);

	REGISTER_NS_LONG_CONSTANT("utypes", "VERIFY_PARAM", PHP_UTYPES_HANDLER_FLAG_PARAM, CONST_CS | CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("utypes", "VERIFY_RETURN", PHP_UTYPES_HANDLER_FLAG_RETURN, CONST_CS | CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("utypes", "VERIFY_INSTANCEOF", PHP_UTYPES_HANDLER_FLAG_INSTANCEOF, CONST_CS | CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("utypes", "VERIFY_NULLABLE", PHP_UTYPES_HANDLER_FLAG_NULLABLE, CONST_CS | CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("utypes", "VERIFY_VARIADIC", PHP_UTYPES_HANDLER_FLAG_VARIADIC, CONST_CS | CONST_PERSISTENT);

	zend_set_user_opcode_handler(ZEND_RECV, php_utypes_handle_op);
	zend_set_user_opcode_handler(ZEND_RECV_INIT, php_utypes_handle_op);
	zend_set_user_opcode_handler(ZEND_RECV_VARIADIC, php_utypes_handle_op);
	zend_set_user_opcode_handler(ZEND_VERIFY_RETURN_TYPE, php_utypes_handle_op);

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION */
PHP_MSHUTDOWN_FUNCTION(utypes)
{
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RINIT_FUNCTION */
PHP_RINIT_FUNCTION(utypes)
{
#if defined(COMPILE_DL_UTYPES) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif

	php_utypes_handler_ctor(&UTG(default_handler));
	UTG(busy) = 0;

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RSHUTDOWN_FUNCTION */
PHP_RSHUTDOWN_FUNCTION(utypes)
{
	if (UTYPES_HAVE_DEFAULT_HANDLER()) {
		zval_dtor(&UTG(callback));
	}

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION */
PHP_MINFO_FUNCTION(utypes)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "utypes support", "enabled");
	php_info_print_table_end();
}
/* }}} */

ZEND_BEGIN_ARG_INFO_EX(php_utypes_handler_arginfo, 0, 0, 1)
	ZEND_ARG_CALLABLE_INFO(0, callback, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(php_utypes_verify_arginfo, 0, 0, 2)
	ZEND_ARG_CALLABLE_INFO(0, function, 0)
	ZEND_ARG_TYPE_INFO(0, interface, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* {{{ utypes_functions[] */
const zend_function_entry utypes_functions[] = {
	ZEND_NS_FE("utypes", handler,	php_utypes_handler_arginfo)
	ZEND_NS_FE("utypes", verify,	php_utypes_verify_arginfo)
	PHP_FE_END
};
/* }}} */

/* {{{ utypes_module_entry */
zend_module_entry utypes_module_entry = {
	STANDARD_MODULE_HEADER,
	"utypes",
	utypes_functions,
	PHP_MINIT(utypes),
	PHP_MSHUTDOWN(utypes),
	PHP_RINIT(utypes),
	PHP_RSHUTDOWN(utypes),
	PHP_MINFO(utypes),
	PHP_UTYPES_VERSION,
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_UTYPES
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(utypes)
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
