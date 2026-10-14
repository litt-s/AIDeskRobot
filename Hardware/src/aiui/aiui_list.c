#include <string.h>
#include "aiui_wrappers_os.h"
#include "aiui_list.h"

aiui_list_t *aiui_list_create()
{
    aiui_list_t *head = (aiui_list_t *)aiui_hal_malloc(sizeof(aiui_list_t));
    head->head = head;
    head->tail = head;
    head->next = NULL;
    head->val = NULL;
    head->len = 0;
    head->size = 0;
    return head;
}

void aiui_list_push(aiui_list_t *head, void *val, int len)
{
    if (NULL == head) return;
    static int count = 0;
    aiui_list_t *node = (aiui_list_t *)aiui_hal_malloc(sizeof(aiui_list_t));
    count++;
    if (len > 0) {
        node->val = aiui_hal_malloc(len);
        count++;
        memcpy(node->val, val, len);
        head->size += len;
    } else {
        node->val = NULL;
    }
    node->next = NULL;
    node->len = len;
    head->tail->next = node;
    head->tail = node;
    head->len += 1;
}

aiui_list_t *aiui_list_pop(aiui_list_t *head)
{
    if (NULL == head || head->len <= 0) return NULL;
    aiui_list_t *pop = head->next;
    head->len -= 1;
    if (head->len != 0)
        head->next = head->next->next;
    else {
        head->tail = head;
        head->next = NULL;
    }
    return pop;
}

void aiui_list_node_destroy(aiui_list_t *node)
{
    if (NULL == node) {
        return;
    }
    static int count = 0;
    if (NULL != node->val) {
        aiui_hal_free(node->val);
        count++;
        node->val = NULL;
    }
    aiui_hal_free(node);
    count++;
}

void aiui_list_destroy(aiui_list_t *head)
{
    if (NULL == head) return;
    aiui_list_t *cur = head->next;
    aiui_list_t *next = NULL;
    while (cur != NULL) {
        next = cur->next;
        aiui_list_node_destroy(cur);
        cur = next;
    }
    aiui_hal_free(head);
}