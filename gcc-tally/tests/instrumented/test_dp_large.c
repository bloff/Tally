/*
 * Instrumented stress test for large dynamic-programming style control flow
 * under the Tally GCC plugin.
 */
#include <stdio.h>
#include <stdint.h>

extern int64_t counter;

typedef long long ll;

#define INF32 1e9
#define for0(i,a) for(ll i = 0; i < (ll)a; i++)

int n;
int dp[105][105][2];
int arr[105];

ll max(ll a, ll b){
    return a > b ? a : b;    
}

int rec(int l, int r, int player){
	if(r < l) return 0;
	if(dp[l][r][player] != -INF32) return dp[l][r][player];

	int sum = 0;
	for(int i = l; i <= r; i++){
		sum += arr[i];
		dp[l][r][player] = max(dp[l][r][player], sum - rec(i+1, r, 1-player));
	}

	sum = 0;
	for(int i = r; i >= l; i--){
		sum += arr[i];
		dp[l][r][player] = max(dp[l][r][player], sum - rec(l, i - 1, 1 - player));
	}

	return dp[l][r][player];
}

void solve(){
	scanf("%d", &n);
	if(n == 0) return;
	for(int k = 0; k < 2; k++){
		for(int i = 0; i < n; i++){
			for(int j = 0; j < n; j++) dp[i][j][k] = -INF32;
		}
	}
	for0(i, n) scanf("%d", arr + i);

    printf("%d\n", rec(0, n-1, 0));
    
    solve();
}
