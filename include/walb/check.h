/**
 * check and related macros.
 *
 * @author HOSHINO Takashi <hoshino@labs.cybozu.co.jp>
 *
 * (C) 2013 Cybozu Labs, Inc.
 */
#ifndef WALB_CHECK_H
#define WALB_CHECK_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Check macro for is_valid_* functions.
 */
#define WALB_CHECK(label, cond, msg, level) do {	\
		if (!(cond)) {				\
			goto label;			\
		}					\
	} while (0)

#define CHECKd(cond) WALB_CHECK(error, cond, "", d)
#define CHECKi(cond) WALB_CHECK(error, cond, "", i)
#define CHECKn(cond) WALB_CHECK(error, cond, "", n)
#define CHECKw(cond) WALB_CHECK(error, cond, "", w)
#define CHECKe(cond) WALB_CHECK(error, cond, "", e)

#define CHECKld(label, cond) WALB_CHECK(label, cond, "", d)
#define CHECKli(label, cond) WALB_CHECK(label, cond, "", i)
#define CHECKln(label, cond) WALB_CHECK(label, cond, "", n)
#define CHECKlw(label, cond) WALB_CHECK(label, cond, "", w)
#define CHECKle(label, cond) WALB_CHECK(label, cond, "", e)

#ifdef __cplusplus
}
#endif

#endif /* WALB_CHECK_H */
