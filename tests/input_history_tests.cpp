#include <gtest/gtest.h>

#include "input_history.h"

namespace {

TEST(InputHistoryTest, MixedEntriesDraftRestoreClearAndCopyOnEdit) {
    InputHistory history;
    history.previous();
    history.next();
    EXPECT_TRUE(history.current().empty());
    history.edit("hello");
    history.submit();
    history.edit("/help");
    history.submit();
    history.edit("unfinished chat");
    history.previous();
    EXPECT_EQ(history.current(), "/help");
    history.previous();
    history.previous();
    EXPECT_EQ(history.current(), "hello");
    history.next();
    EXPECT_EQ(history.current(), "/help");
    history.next();
    EXPECT_EQ(history.current(), "unfinished chat");
    history.next();
    EXPECT_TRUE(history.current().empty());
    history.previous();
    history.previous();
    history.edit("hello edited");
    EXPECT_EQ(history.current(), "hello edited");
    history.previous();
    EXPECT_EQ(history.current(), "/help");
    history.previous();
    EXPECT_EQ(history.current(), "hello");
    history.next();
    history.next();
    EXPECT_EQ(history.current(), "hello edited");
    history.submit();
    history.previous();
    EXPECT_EQ(history.current(), "hello edited");
}

TEST(InputHistoryTest, SubmissionWithoutEditingAndBoundedStorage) {
    InputHistory history;
    for (int i = 0; i < 105; ++i) {
        history.edit(std::to_string(i));
        history.submit();
    }
    for (int i = 0; i < 110; ++i) history.previous();
    EXPECT_EQ(history.current(), "5");
    history.submit();
    EXPECT_TRUE(history.current().empty());
    history.previous();
    EXPECT_EQ(history.current(), "5");
}

}  // namespace
