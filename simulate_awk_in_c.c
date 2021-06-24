#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <regex.h>

#define AWK_OK                  0
#define AWK_CONTINUE            1
#define AWK_BREAK               2
#define AWK_FIELD_OUTOFRANGE    3
#define AWK_LINE_OUTOFRANGE     4
#define AWK_OPEN_FAILED         5
#define AWK_REGCOMP             -1
#define AWK_UNMATCH             -2

/*
 * BSD license.
 *
 * awk related paramters are put in struct awk_st, see the defination. put your private data next to struct awk_st and pack them as the paramter of awk.
 * fun_begin, fun_line and fun_end are the function for when begin , every line and end. if they are unused pass NULL to the function.
 * data->data are user defined type for private use inside the functions, change the type to what it is in the functions.
 * pattern_num must be set and when multipattern the first match is used. if match all, use "". 
 * pattern and action should be in pairs, if no pattern, pattern_default or pattern[0] should be set.
 * if fields[0] is set to AWK_FIELD0_USED or delim is an empty string, $0 will be saved in fields[0], otherwise $0 will be an empty string.
 * all unused fields are set to empty by default.
 *
 * fun_end return none value, while the fun_begin and fun_line
 * return AWK_CONTINUE to continue, return AWK_BREAK to stop excution.
 * fun_end is always excuted even when meets AWK_BREAK.
 *
 * if success awk return AWK_OK, otherwise an errno that can be reported by awk_errno is returned.
 *
 * the temporary fields and line will be unavaliable if they are in an stack and when the stack is unavaliable.
 * modify line and fields to satify your use, an example is appended to the code.
 *
 * Note: the return value that the caller wanted should be stored in data.
 *       what fun_begin and func_line return are just for awk, and what awk return is only used for reporting awk errors.
 *
 * some replace function for string are provided, see the definations.
 *
 */

typedef int (*awk_begin_t)(void *data);
typedef int (*awk_action_t)(int row_idx, char *fields[], int num_of_fields, void *data);
typedef void (*awk_end_t)(int row_idx, char *fields[], int num_of_fields, void *data);
struct awk_st
{
#define PATTERN_NUM 3
#define PATTERN_SIZE 16
    int pattern_num;
    char pattern[PATTERN_NUM][PATTERN_SIZE];
    awk_begin_t fun_begin;
    awk_end_t fun_end;
    char action_default[0];
    awk_action_t actions[PATTERN_NUM];
    char data[0];
};

#define AWK_FIELD0_USED (void*)-1
int awk_(const char *filename, const char *delim, char line[], int linesize, char *fields[], int fieldnum, struct awk_st *_data);
    /* modify it before use */
int awk(const char *filename, const char *delim, struct awk_st *_data);
const char *awk_error(int err);

/* not found also return 0 */
int awk_str_replace_inplace(char *src, const char *old, const char *new);
int awk_str_replace(const char *src, const char *old, const char *new, char buf[], int bufsize);
int awk_str_replace_regex(const char *src, const char *pattern, const char *new, char buf[], int bufsize);
int awk_str_replace_regex_inplace(char *src, const char *pattern, const char *new);



const char *awk_error(int err)
{
    switch(err)
    {
        case AWK_FIELD_OUTOFRANGE:
            return "fields is too small";
        case AWK_LINE_OUTOFRANGE:
            return "line is too small";
        case AWK_OPEN_FAILED:
            return "open file failed";
        case AWK_REGCOMP:
            return "regcomp failed";
        default:
            return "unknown failed";
    }
}

/* not found also return 0 */
int awk_str_replace_inplace(char *src, const char *old, const char *new)
{
    int old_len = strlen(old);
    int new_len = strlen(new);
    char *begin;

    if(new_len > old_len)
        return -1;

    begin = strstr(src, old);
    if(begin == NULL)
        return 0;

    memcpy(begin, new, new_len);
    if(new_len != old_len)
        strcpy(&begin[new_len], &begin[old_len]);

    return 0;
}

/* not found also return 0 */
int awk_str_replace(const char *src, const char *old, const char *new, char buf[], int bufsize)
{
    int srclen = strlen(src);
    int old_len = strlen(old);
    int new_len = strlen(new);
    char *begin;

    if(srclen-old_len+new_len+1 > bufsize)
        return -1;

    begin = strstr(src, old);
    if(begin == NULL)
        return 0;

    memcpy(buf, src, begin-src);
    memcpy(&buf[begin-src], new, new_len);
    memcpy(&buf[begin-src+new_len], begin+old_len, srclen+1-old_len-(begin-src));
    return 0;
}

/* not found also return 0 */
int awk_str_replace_regex(const char *src, const char *pattern, const char *new, char buf[], int bufsize)
{
    regex_t preg;
    regmatch_t  pmatch[1];
    int srclen = strlen(src);
    int new_len = strlen(new);

    if(regcomp(&preg, pattern, 0|REG_EXTENDED) != 0)
        return -2;

    int ret = regexec(&preg, src, 1, pmatch, 0);
    regfree(&preg);
    if(ret != 0)
    {
        return 0;
    }

    if(srclen-(pmatch[0].rm_eo - pmatch[0].rm_so)+new_len+1 > bufsize)
        return -1;

    memcpy(buf, src, pmatch[0].rm_so);
    memcpy(&buf[pmatch[0].rm_so], new, new_len);
    memcpy(&buf[pmatch[0].rm_so+new_len], src+pmatch[0].rm_eo, srclen+1-pmatch[0].rm_eo);

    return 0;
}
/* not found also return 0 */
int awk_str_replace_regex_inplace(char *src, const char *pattern, const char *new)
{
    regex_t preg;
    regmatch_t  pmatch[1];
    int new_len = strlen(new);

    if(regcomp(&preg, pattern, 0|REG_EXTENDED) != 0)
        return -2;

    int ret = regexec(&preg, src, 1, pmatch, 0);
    regfree(&preg);
    if(ret != 0)
    {
        return 0;
    }

    if(new_len > pmatch[0].rm_eo - pmatch[0].rm_so)
        return -1;

    memcpy(src+pmatch[0].rm_so, new, new_len);

    if(new_len != pmatch[0].rm_eo - pmatch[0].rm_so)
        strcpy(src+pmatch[0].rm_so+new_len, src+pmatch[0].rm_eo);

    return 0;
}

#define call_with_inputfile(filename, f, argv...) \
    ({\
        int ret;\
        FILE *stream;\
        stream = fopen(filename, "r");\
        if (stream == NULL) {\
            ret = -1;\
        }\
        ret = f(stream, ##argv);\
        fclose(stream);\
        ret;\
    })

/* match the first */
int awk_match(struct awk_st *data, const char *line)
{
    int i;
    int pattern_num = data->pattern_num;

    if(pattern_num == 0)    /* use default action */
        return 0;

    for (i = 0; i < pattern_num; ++i) {
        int ret;
        regex_t preg;
        regmatch_t  pmatch[1];

        if(data->pattern[i] && data->pattern[i][0] == 0)    /* "" match all line */
            return i;

        if(regcomp(&preg, data->pattern[i], 0|REG_EXTENDED) != 0)
            return AWK_REGCOMP;

        ret = regexec(&preg, line, 1, pmatch, 0);

        //size_t regerror(int errcode, const regex_t *preg, char *errbuf,
        //       size_t errbuf_size);

        regfree(&preg);
        if(ret == 0)
            return  i;
    }
    return AWK_UNMATCH;
}
int awk__(FILE *stream, const char *delim, char line[], int linesize, char *fields[], int fieldnum, struct awk_st *_data)
{
#define ADD_FIELD(found) \
        if(found)\
        do{\
            if(field_idx >= fieldnum)\
                return AWK_FIELD_OUTOFRANGE;\
            fields[field_idx++]=&line[i];\
            found = 0;\
        }while(0)

    awk_begin_t fun_begin = _data->fun_begin;
    awk_end_t fun_end = _data->fun_end;
    awk_action_t *actions = _data->actions;
    void *data = _data->data;
    

    int row_idx = 0;
    int i, field_idx, found, field0_used;

    if(fieldnum < 1)                    /* at least one field */
        return AWK_FIELD_OUTOFRANGE;

    if(fun_begin)
    {
        if(fun_begin(data) != AWK_CONTINUE)
            return AWK_OK;
    }

    if(fields[0] == AWK_FIELD0_USED || *delim == 0)
        field0_used = 1;
    else
        field0_used = 0;

    for (i = 0; i < fieldnum; ++i) {
        static char *empty="";
        fields[i] = empty;
    }
    errno = 0;
    while(fgets(line, linesize, stream) || errno == EINTR)
    {
        int act_idx;
        if((act_idx=awk_match(_data, line)) < 0)
        {
            if(act_idx == AWK_UNMATCH)
                continue;
            return act_idx;     /* error number */
        }
        awk_action_t fun_action = actions[act_idx];
        if(errno == EINTR)
        {
            errno = 0;
            continue;
        }

        for (i = 0; i < fieldnum; ++i) {
            static char *empty="";
            fields[i] = empty;
        }
        field_idx = 1; 
        if(*delim)
        {
            if(field0_used)
            {
                int l = strlen(line);
                if((l*2) > linesize-2)
                    return AWK_LINE_OUTOFRANGE;
                fields[0] = &line[l+1];
                strcpy(fields[0], &line[0]);
            }
            for(i = 0,found=1; i < linesize
                    && line[i] != '\n' && line[i] != 0; i++)
            {
                ADD_FIELD(found);
                if(strchr(delim, line[i]))
                {
                    line[i] = 0;
                    found = 1;
                }
            }
            if(i == linesize)
                return AWK_LINE_OUTOFRANGE;
            line[i] = 0; // if '\n', -> '\0'
            ADD_FIELD(found);
        }
        else
        {
            fields[0] = line;
        }

        if(fun_action)
        {
            if(fun_action(row_idx, fields, field_idx, data) != AWK_CONTINUE)
                return AWK_OK;
        }

        row_idx++;
    }

    if(fun_end)
    {
        fun_end(row_idx, fields, field_idx, data);
    }
    return AWK_OK;
}

int awk_(const char *filename, const char *delim, char line[], int linesize, char *fields[], int fieldnum, struct awk_st *_data)
{
    int ret;
    ret = call_with_inputfile(filename, awk__, delim, line, linesize, fields, fieldnum, _data);
    if(ret < 0)
        return AWK_OPEN_FAILED;
    return ret;
}
int awk(const char *filename, const char *delim, struct awk_st *_data)
{
    char line[5120];
    char *fields[10];
    fields[0] = AWK_FIELD0_USED;
    return awk_(filename, delim, line, sizeof line, fields, sizeof fields/sizeof fields[0], _data);
}








/* below is an example of how to use
 * it will print the $1 and $0 of each line in /etc/passwd with delim : */


struct buf_st
{
    char buf[1024];
    int i;
    int ret;
};
int func_begin(void *data)
{
    struct buf_st *buf = data;
    int i, ret;
    i = buf->i;
    ret = snprintf(&buf->buf[i], sizeof buf->buf - i, "users are: \n");
    if(ret < 0)
    {
        buf->ret = ret;
        return AWK_BREAK;
    }
    buf->i += ret;
    if(buf->i >= sizeof buf->buf)
    {
        buf->i = sizeof buf->buf;
        return AWK_BREAK;
    }
    return AWK_CONTINUE;
}
void func_end(int row_idx, char *fields[], int num_of_fields, void *data)
{
    struct buf_st *buf = data;
    int i, ret;
    i = buf->i;
    (void)fields;
    (void)num_of_fields;
    ret = snprintf(&buf->buf[i], sizeof buf->buf - i, "\n total num: %d\n", row_idx);
    if(ret < 0)
    {
        buf->ret = ret;
        return;
    }
    buf->i += ret;
    if(buf->i >= sizeof buf->buf)
    {
        buf->i = sizeof buf->buf;
    }
    buf->ret = 0;
}
int func_action(int row_idx, char *fields[], int num_of_fields, void *data)
{
    struct buf_st *buf = data;
    int i, ret;
    (void) num_of_fields;
    i = buf->i;
    ret = snprintf(&buf->buf[i], sizeof buf->buf - i, "\t %d. %s %s\n", row_idx, fields[1], fields[0]);
    if(ret < 0)
    {
        buf->ret = ret;
        return AWK_BREAK;
    }
    buf->i += ret;
    if(buf->i >= sizeof buf->buf)
    {
        buf->i = sizeof buf->buf;
        return AWK_BREAK;
    }
    return AWK_CONTINUE;
}

void example(void)
{
    struct buf_all
    {
        struct awk_st awk;
        struct buf_st buf;
    };
    struct buf_all buf = {{0}};

    buf.awk.pattern_num = 2;
    strcpy(buf.awk.pattern[0], "d*.nal");
    strcpy(buf.awk.pattern[1], "root");
    buf.awk.fun_begin = func_begin;
    buf.awk.fun_end = func_end;
    buf.awk.actions[0] = func_action;
    buf.awk.actions[1] = func_action;
    int ret = awk("/etc/passwd", ":", (struct awk_st*)&buf);
    if(ret != AWK_OK)
    {
        fprintf(stderr, "awk wrong:%s\n", awk_error(ret));
    }
    fprintf(stdout, "%s", buf.buf.buf);
}
int main(void)
{
    example();
    return 0;
}


